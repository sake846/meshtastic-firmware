#include "configuration.h"
#if defined(HAS_QZSS_DCR)
#if HAS_SCREEN || defined(MESHTASTIC_INCLUDE_NICHE_GRAPHICS)
#include "MessageStore.h"
#endif
#include "QzssDcr.h"
#include "FSCommon.h"
#include "SafeFile.h"
#include "SPILock.h"
#include "UptimeClock.h"
#include "gps/RTC.h"
#include "mesh/Channels.h"
#include "mesh/MeshService.h"
#include "mesh/NodeDB.h"
#include "mesh/Router.h"
#include "mesh/Throttle.h"
#include "mesh/TypeConversions.h"
#include "modules/ExternalNotificationModule.h"
#ifdef MESHTASTIC_ENCRYPTED_STORAGE
#include "security/EncryptedStorage.h"
#endif
#include <atomic>
#include <cstdio>
#include <cstring>

namespace QzssDcr
{
namespace
{
constexpr char CHANNEL_NAME[] = "QZSS-DCR";
constexpr uint8_t NO_CHANNEL = UINT8_MAX;
constexpr uint16_t MAX_UBX_PAYLOAD = 1024;
constexpr uint32_t CRC24Q_POLY = 0x864CFB;
constexpr uint32_t CRC24_MASK = 0xFFFFFF;
constexpr uint8_t L1S_PREAMBLES[] = {0x53, 0x9A, 0xC6};
constexpr uint32_t SETTINGS_MAGIC = 0x515A4C31;
constexpr uint8_t SETTINGS_VERSION = 1;
constexpr uint32_t CAPTURE_TIMEOUT_MS = 15000;
constexpr uint32_t DIAGNOSTIC_LOG_INTERVAL_MS = 30000;
constexpr uint32_t NODE_HEARD_UPDATE_INTERVAL_MS = 10000;
constexpr uint8_t MAX_MICHIBIKI_SV_ID = MAX_QZSS_SATELLITES;
std::atomic<uint8_t> diagnosticQzssCno[MAX_MICHIBIKI_SV_ID] = {};
std::atomic<uint8_t> lastQzssCno[MAX_MICHIBIKI_SV_ID] = {};
std::atomic<uint32_t> lastL1SBySatelliteMs[MAX_MICHIBIKI_SV_ID] = {};
std::atomic<uint8_t> lastMessageTypeBySatellite[MAX_MICHIBIKI_SV_ID] = {};
std::atomic<bool> hasL1SBySatellite[MAX_MICHIBIKI_SV_ID] = {};
uint32_t lastNodeHeardUpdateMs[MAX_MICHIBIKI_SV_ID] = {};
bool hasNodeHeardUpdate[MAX_MICHIBIKI_SV_ID] = {};

uint8_t channelIndex = NO_CHANNEL;
uint32_t recentFrames[16] = {};
uint8_t recentFrameIndex = 0;
std::atomic<bool> enabled{true};
std::atomic<bool> m10Detected{false};
std::atomic<bool> settingsLoaded{false};
std::atomic<bool> parserResetPending{false};
std::atomic<bool> hasCapturedSatellite{false};
std::atomic<uint32_t> lastCaptureMs{0};
std::atomic<uint8_t> lastCaptureSvId{0};
std::atomic<bool> hasSatelliteReport{false};
std::atomic<uint32_t> lastSatelliteReportMs{0};
uint32_t lastDiagnosticLogMs = 0;
uint32_t sfrbxMessages = 0;
uint32_t sfrbxChecksumErrors = 0;
uint32_t sfrbxOversizeMessages = 0;
uint32_t sfrbxLengthErrors = 0;
uint32_t nonL1sSfrbxMessages = 0;
uint32_t l1sSfrbxMessages = 0;
uint32_t l1sWordCountErrors = 0;
uint32_t l1sPreambleRejects = 0;
uint32_t l1sPreambleMatches = 0;
uint32_t l1sCrcRejects = 0;
uint32_t l1sCrcMatches = 0;
uint32_t l1sOtherMessageTypes = 0;
uint32_t l1sMt43Messages = 0;
uint32_t l1sMt44Messages = 0;
uint32_t l1sMt63Messages = 0;
uint32_t l1sFramesBySatellite[MAX_MICHIBIKI_SV_ID] = {};
uint32_t lastValidL1SMs = 0;
uint32_t lastMt43Or44Ms = 0;
bool hasValidL1S = false;
bool hasMt43Or44 = false;

struct SettingsRecord {
    uint32_t magic;
    uint8_t version;
    uint8_t enabled;
    uint16_t reserved;
};

void loadSettings()
{
    if (settingsLoaded.exchange(true))
        return;

#ifdef FSCom
    {
        concurrency::LockGuard guard(spiLock);
        if (!FSCom.exists(SETTINGS_FILE_NAME))
            return;
    }

    SettingsRecord record = {};
    size_t bytesRead = 0;
#ifdef MESHTASTIC_ENCRYPTED_STORAGE
    if (EncryptedStorage::isLockdownActive()) {
        if (!EncryptedStorage::readAndDecrypt(SETTINGS_FILE_NAME, reinterpret_cast<uint8_t *>(&record), sizeof(record), bytesRead))
            return;
    } else
#endif
    {
        concurrency::LockGuard guard(spiLock);
        auto file = FSCom.open(SETTINGS_FILE_NAME, FILE_O_READ);
        if (!file)
            return;
        bytesRead = file.read(reinterpret_cast<uint8_t *>(&record), sizeof(record));
        file.close();
    }

    if (bytesRead == sizeof(record) && record.magic == SETTINGS_MAGIC && record.version == SETTINGS_VERSION &&
        record.reserved == 0)
        enabled.store(record.enabled != 0);
#endif
}

bool saveSettings()
{
#ifdef FSCom
    const SettingsRecord record = {SETTINGS_MAGIC, SETTINGS_VERSION, static_cast<uint8_t>(enabled.load()), 0};
#ifdef MESHTASTIC_ENCRYPTED_STORAGE
    if (EncryptedStorage::isLockdownActive()) {
        if (!EncryptedStorage::isUnlocked()) {
            LOG_WARN("Can't save QZSS L1S setting while storage is locked");
            return false;
        }
        return EncryptedStorage::encryptAndWrite(SETTINGS_FILE_NAME, reinterpret_cast<const uint8_t *>(&record), sizeof(record),
                                                 true);
    }
#endif
    {
        concurrency::LockGuard guard(spiLock);
        FSCom.mkdir("/prefs");
    }
    auto file = SafeFile(SETTINGS_FILE_NAME, true);
    return file.write(reinterpret_cast<const uint8_t *>(&record), sizeof(record)) == sizeof(record) && file.close();
#else
    return true;
#endif
}

bool isEmptyChannelSettings(const meshtastic_ChannelSettings &settings)
{
    return settings.channel_num == 0 && settings.psk.size == 0 && settings.name[0] == '\0' && settings.id == 0 &&
           !settings.uplink_enabled && !settings.downlink_enabled && !settings.has_module_settings &&
           settings.module_settings.position_precision == 0 && !settings.module_settings.is_muted && !settings.use_aead;
}

enum class ParserState : uint8_t { Sync1, Sync2, Class, Id, LengthLow, LengthHigh, Payload, ChecksumA, ChecksumB };

struct UbxParser {
    ParserState state = ParserState::Sync1;
    uint8_t messageClass = 0;
    uint8_t messageId = 0;
    uint8_t checksumA = 0;
    uint8_t checksumB = 0;
    uint8_t receivedChecksumA = 0;
    uint16_t length = 0;
    uint16_t position = 0;
    uint8_t payload[MAX_UBX_PAYLOAD] = {};
    bool payloadTooLarge = false;

    void reset()
    {
        state = ParserState::Sync1;
        length = 0;
        position = 0;
        payloadTooLarge = false;
    }

    void checksum(uint8_t byte)
    {
        checksumA = static_cast<uint8_t>(checksumA + byte);
        checksumB = static_cast<uint8_t>(checksumB + checksumA);
    }
};

UbxParser parser;

uint8_t getBit(const uint8_t *frame, uint16_t bit)
{
    return static_cast<uint8_t>((frame[bit / 8] >> (7 - bit % 8)) & 1);
}

uint32_t getBits(const uint8_t *frame, uint16_t firstBit, uint8_t count)
{
    uint32_t value = 0;
    for (uint8_t i = 0; i < count; ++i)
        value = (value << 1) | getBit(frame, firstBit + i);
    return value;
}

uint32_t crc24q(const uint8_t *frame)
{
    uint32_t crc = 0;
    for (uint16_t bit = 0; bit < 226; ++bit) {
        const bool feedback = ((crc >> 23) & 1) != getBit(frame, bit);
        crc = (crc << 1) & CRC24_MASK;
        if (feedback)
            crc ^= CRC24Q_POLY;
    }
    return crc;
}

std::atomic<uint16_t> qzssSatelliteData[MAX_MICHIBIKI_SV_ID] = {};

// These local-only identities attribute DCR chat without making satellites radio peers.
bool isMichibikiSatelliteNode(const meshtastic_NodeInfoLite *node, uint8_t svId)
{
    if (!node || node->num != MICHIBIKI_NODE_NUM_BASE + svId || !nodeInfoLiteHasUser(node) ||
        node->public_key.size != 0 || !nodeInfoLiteHasIsUnmessagable(node) || !nodeInfoLiteIsUnmessagable(node))
        return false;

    char expectedName[sizeof(node->long_name)];
    std::snprintf(expectedName, sizeof(expectedName), "Michibiki SV%u", static_cast<unsigned>(svId));
    return std::strcmp(node->long_name, expectedName) == 0;
}

bool registerMichibikiSatellite(uint8_t svId)
{
    if (!nodeDB || svId == 0 || svId > MAX_MICHIBIKI_SV_ID)
        return false;

    const NodeNum nodeNum = MICHIBIKI_NODE_NUM_BASE + svId;
    const meshtastic_NodeInfoLite *existing = nodeDB->getMeshNode(nodeNum);
    if (existing && !isMichibikiSatelliteNode(existing, svId))
        return false;
    if (!existing && nodeDB->isFull())
        return false;

    meshtastic_User satellite = meshtastic_User_init_zero;
    std::snprintf(satellite.long_name, sizeof(satellite.long_name), "Michibiki SV%u", static_cast<unsigned>(svId));
    std::snprintf(satellite.short_name, sizeof(satellite.short_name), "Q%u", static_cast<unsigned>(svId));
    satellite.hw_model = meshtastic_HardwareModel_UNSET;
    satellite.role = meshtastic_Config_DeviceConfig_Role_CLIENT;
    satellite.has_is_unmessagable = true;
    satellite.is_unmessagable = true;
    nodeDB->updateUser(nodeNum, satellite, channelIndex);
    return isMichibikiSatelliteNode(nodeDB->getMeshNode(nodeNum), svId);
}

void sendMichibikiSatelliteIdentityToPhone(uint8_t svId)
{
    if (!nodeDB || !router || !service || svId == 0 || svId > MAX_MICHIBIKI_SV_ID)
        return;

    const NodeNum nodeNum = MICHIBIKI_NODE_NUM_BASE + svId;
    const auto *node = nodeDB->getMeshNode(nodeNum);
    if (!isMichibikiSatelliteNode(node, svId))
        return;

    meshtastic_MeshPacket *packet = router->allocForSending();
    if (!packet)
        return;

    const meshtastic_User satellite = TypeConversions::ConvertToUser(node);
    packet->from = nodeNum;
    packet->to = 0xffffffff;
    packet->channel = channelIndex < MAX_NUM_CHANNELS ? channelIndex : 0;
    if (!packet->has_rx_time)
        packet->rx_time = Time::getUptimeSecs();
    packet->decoded.portnum = meshtastic_PortNum_NODEINFO_APP;
    packet->decoded.want_response = false;
    packet->decoded.payload.size = pb_encode_to_bytes(packet->decoded.payload.bytes, sizeof(packet->decoded.payload.bytes),
                                                      &meshtastic_User_msg, &satellite);
    if (!packet->decoded.payload.size) {
        service->releaseToPool(packet);
        return;
    }
    service->sendToPhone(packet);
}

void recordMichibikiSatelliteReception(uint8_t svId)
{
    if (svId == 0 || svId > MAX_MICHIBIKI_SV_ID)
        return;

    const uint8_t index = svId - 1;
    if (hasNodeHeardUpdate[index] &&
        !Throttle::hasElapsed(lastNodeHeardUpdateMs[index], NODE_HEARD_UPDATE_INTERVAL_MS))
        return;

    lastNodeHeardUpdateMs[index] = Time::stampMillis();
    hasNodeHeardUpdate[index] = true;
    meshtastic_MeshPacket packet = meshtastic_MeshPacket_init_zero;
    packet.which_payload_variant = meshtastic_MeshPacket_decoded_tag;
    packet.from = MICHIBIKI_NODE_NUM_BASE + svId;
    packet.rx_time = getValidTime(RTCQualityFromNet);
    packet.has_rx_time = packet.rx_time != 0;
    if (!packet.has_rx_time)
        packet.rx_time = Time::getUptimeSecs();
    nodeDB->updateFrom(packet);
    sendMichibikiSatelliteIdentityToPhone(svId);
}

uint32_t frameHash(const uint8_t *frame)
{
    uint32_t hash = 2166136261UL;
    for (uint16_t bit = 8; bit < 226; ++bit) {
        hash ^= (frame[bit / 8] >> (7 - bit % 8)) & 1;
        hash *= 16777619UL;
    }
    return hash;
}

bool seenRecently(uint32_t hash)
{
    for (uint32_t recent : recentFrames)
        if (recent == hash)
            return true;
    recentFrames[recentFrameIndex++ % (sizeof(recentFrames) / sizeof(recentFrames[0]))] = hash;
    return false;
}

bool appendDcxText(char *message, size_t capacity, size_t &position, const char *text)
{
    const size_t length = std::strlen(text);
    if (position >= capacity || length >= capacity - position)
        return false;
    std::memcpy(message + position, text, length);
    position += length;
    message[position] = '\0';
    return true;
}

bool appendDcxNumber(char *message, size_t capacity, size_t &position, unsigned value)
{
    if (position >= capacity)
        return false;
    const int length = std::snprintf(message + position, capacity - position, "%u", value);
    if (length < 0 || static_cast<size_t>(length) >= capacity - position)
        return false;
    position += static_cast<size_t>(length);
    return true;
}

const char *dcxHazardText(uint32_t code)
{
    switch (code) {
    case 8:
        return "ミサイル攻撃";
    case 27:
        return "森林火災";
    case 36:
        return "地震";
    case 44:
        return "津波";
    case 68:
        return "洪水";
    case 74:
        return "大雨";
    case 82:
        return "台風";
    case 95:
        return "安全上の警報";
    case 113:
        return "試験情報";
    default:
        return nullptr;
    }
}

const char *dcxSeverityText(uint32_t severity)
{
    switch (severity) {
    case 0:
        return "不明";
    case 1:
        return "中程度";
    case 2:
        return "重大";
    case 3:
        return "極めて重大";
    default:
        return "未定義";
    }
}

const char *dcrReportClassText(uint32_t reportClass)
{
    switch (reportClass) {
    case 1:
        return "最優先";
    case 2:
        return "優先";
    case 3:
        return "通常";
    case 7:
        return "訓練・試験";
    default:
        return nullptr;
    }
}

const char *dcrHazardText(uint32_t hazard)
{
    switch (hazard) {
    case 1:
        return "緊急地震速報";
    case 2:
        return "震源情報";
    case 3:
        return "震度情報";
    case 4:
        return "東海地震情報";
    case 5:
        return "津波情報";
    case 6:
        return "北西太平洋津波情報";
    case 8:
        return "火山情報";
    case 10:
        return "気象情報";
    case 11:
        return "洪水情報";
    case 12:
        return "台風情報";
    case 14:
        return "海上警報";
    default:
        return nullptr;
    }
}

bool formatDcrMessage(const uint8_t *frame, char *message, size_t capacity, size_t &position)
{
    const uint32_t reportClass = getBits(frame, 14, 3);
    const uint32_t hazard = getBits(frame, 17, 4);
    const uint32_t reportedAt = getBits(frame, 21, 20);
    const uint32_t infoType = getBits(frame, 41, 2);
    const uint32_t eventId = getBits(frame, 43, 10);
    const char *reportClassText = dcrReportClassText(reportClass);
    const char *hazardText = dcrHazardText(hazard);
    static const char *const infoTypes[] = {"発表", "訂正", "取消", "未定義"};

    message[0] = '\0';
    position = 0;
    if (!appendDcxText(message, capacity, position, "みちびき 災危通報\n種別:"))
        return false;
    if (hazardText) {
        if (!appendDcxText(message, capacity, position, hazardText))
            return false;
    } else if (!appendDcxText(message, capacity, position, "コード") ||
               !appendDcxNumber(message, capacity, position, static_cast<unsigned>(hazard))) {
        return false;
    }
    if (!appendDcxText(message, capacity, position, "\n区分:"))
        return false;
    if (reportClassText) {
        if (!appendDcxText(message, capacity, position, reportClassText))
            return false;
    } else if (!appendDcxText(message, capacity, position, "コード") ||
               !appendDcxNumber(message, capacity, position, static_cast<unsigned>(reportClass))) {
        return false;
    }

    if (!appendDcxText(message, capacity, position, "\n情報:") ||
        !appendDcxText(message, capacity, position, infoTypes[infoType]))
        return false;

    const unsigned month = static_cast<unsigned>((reportedAt >> 16) & 0x0F);
    const unsigned day = static_cast<unsigned>((reportedAt >> 11) & 0x1F);
    const unsigned hour = static_cast<unsigned>((reportedAt >> 6) & 0x1F);
    const unsigned minute = static_cast<unsigned>(reportedAt & 0x3F);
    if (month >= 1 && month <= 12 && day >= 1 && day <= 31 && hour <= 23 && minute <= 59) {
        char timeText[32];
        std::snprintf(timeText, sizeof(timeText), "\n発表時刻:%02u/%02u %02u:%02u UTC", month, day, hour, minute);
        if (!appendDcxText(message, capacity, position, timeText))
            return false;
    }
    if (eventId && (!appendDcxText(message, capacity, position, "\n識別番号:") ||
                    !appendDcxNumber(message, capacity, position, static_cast<unsigned>(eventId))))
        return false;
    return true;
}

bool formatDcxInstruction(uint32_t code, char *text, size_t capacity)
{
    switch (code) {
    case 0:
        text[0] = '\0';
        return true;
    case 1:
    case 127:
    case 255:
        std::snprintf(text, capacity, "直ちに命を守る行動をとってください。");
        return true;
    case 126:
        std::snprintf(text, capacity, "これはDCXの試験です。");
        return true;
    case 128:
        std::snprintf(text, capacity, "ミサイル発射。建物内または地下へ避難。");
        return true;
    case 129:
        std::snprintf(text, capacity, "ミサイル通過。避難解除。不審物に近づかず警察・消防へ通報。");
        return true;
    case 130:
        std::snprintf(text, capacity, "ミサイルは海に落下。避難解除。不審物に注意。");
        return true;
    case 131:
        std::snprintf(text, capacity, "ミサイルは日本に飛来しない見込み。避難解除。");
        return true;
    case 132:
        std::snprintf(text, capacity, "直ちに建物内または地下へ避難。");
        return true;
    case 133:
        std::snprintf(text, capacity, "ミサイル迎撃。破片落下のおそれ。屋内へ避難。");
        return true;
    case 134:
        std::snprintf(text, capacity, "ミサイルが周辺に落下。続報に注意し屋内へ避難。");
        return true;
    case 135:
        std::snprintf(text, capacity, "ミサイルは日本に落下しない見込み。避難解除。");
        return true;
    case 136:
        std::snprintf(text, capacity, "これはJアラートのテストです。");
        return true;
    default:
        break;
    }

    const uint32_t action = code >> 8;
    const uint32_t location = code & 0xFF;
    static const char *const actions[] = {"", "留まってください", "移動してください", "近づかないでください"};
    static const char *const locations[] = {"", "頑丈な建物の中または下", "3階以上", "地下", "山", "水辺", "化学物質を扱う建物",
                                            "崖など崩れやすい場所"};
    if (action >= 1 && action <= 3 && location <= 7) {
        if (location == 0)
            std::snprintf(text, capacity, "%s。", actions[action]);
        else if (action == 1)
            std::snprintf(text, capacity, "%sで%s。", locations[location], actions[action]);
        else if (action == 2)
            std::snprintf(text, capacity, "%sへ%s。", locations[location], actions[action]);
        else
            std::snprintf(text, capacity, "%sに%s。", locations[location], actions[action]);
        return true;
    }
    std::snprintf(text, capacity, "行動指示は未対応です（コード%u）。", static_cast<unsigned>(code));
    return true;
}

bool appendDcxTarget(const uint8_t *frame, uint32_t country, uint32_t provider, char *message, size_t capacity, size_t &position)
{
    if (!appendDcxText(message, capacity, position, "対象:"))
        return false;

    if (country == 111 && (provider == 2 || provider == 3)) {
        static const char *const prefectures[] = {"北海道", "青森", "岩手", "宮城", "秋田", "山形", "福島", "茨城", "栃木", "群馬",
                                                  "埼玉",   "千葉", "東京", "神奈川", "新潟", "富山", "石川", "福井", "山梨", "長野",
                                                  "岐阜",   "静岡", "愛知", "三重",   "滋賀", "京都", "大阪", "兵庫", "奈良", "和歌山",
                                                  "鳥取",   "島根", "岡山", "広島",   "山口", "徳島", "香川", "愛媛", "高知", "福岡",
                                                  "佐賀",   "長崎", "熊本", "大分",   "宮崎", "鹿児島", "沖縄"};
        const uint32_t codeListType = getBits(frame, 146, 1);
        if (codeListType == 0) {
            uint8_t count = 0;
            for (uint8_t i = 0; i < 47; ++i)
                count += getBits(frame, 147 + (46 - i), 1) != 0;
            if (count == 47)
                return appendDcxText(message, capacity, position, "全国\n");
            if (count == 0)
                return appendDcxText(message, capacity, position, "指定なし\n");

            uint8_t written = 0;
            for (uint8_t i = 0; i < 47 && written < 3; ++i) {
                if (getBits(frame, 147 + (46 - i), 1) == 0)
                    continue;
                if (written && !appendDcxText(message, capacity, position, "・"))
                    return false;
                if (!appendDcxText(message, capacity, position, prefectures[i]))
                    return false;
                ++written;
            }
            if (count > written) {
                if (!appendDcxText(message, capacity, position, "ほか"))
                    return false;
                if (!appendDcxNumber(message, capacity, position, count - written) ||
                    !appendDcxText(message, capacity, position, "都道府県"))
                    return false;
            }
            return appendDcxText(message, capacity, position, "\n");
        }

        uint8_t written = 0;
        if (!appendDcxText(message, capacity, position, "市区町村 "))
            return false;
        for (uint8_t i = 0; i < 4; ++i) {
            const uint32_t code = getBits(frame, 147 + i * 16, 16);
            if (code == 0)
                continue;
            if (written && !appendDcxText(message, capacity, position, "・"))
                return false;
            char codeText[8];
            std::snprintf(codeText, sizeof(codeText), "%05u", static_cast<unsigned>(code));
            if (!appendDcxText(message, capacity, position, codeText))
                return false;
            ++written;
        }
        return appendDcxText(message, capacity, position, written ? "\n" : "指定なし\n");
    }

    if (country != 111 || (provider != 1 && provider != 4))
        return appendDcxText(message, capacity, position, "地域形式は未対応\n");

    const uint32_t targetCode = getBits(frame, 146, 16);
    if (targetCode != 0) {
        if (!appendDcxText(message, capacity, position, "市区町村コード"))
            return false;
        char codeText[8];
        std::snprintf(codeText, sizeof(codeText), "%05u", static_cast<unsigned>(targetCode));
        if (!appendDcxText(message, capacity, position, codeText))
            return false;
    } else {
        const uint32_t ellipse = getBits(frame, 80, 16) | getBits(frame, 96, 17) | getBits(frame, 113, 5) | getBits(frame, 118, 5) |
                                 getBits(frame, 123, 6);
        if (!appendDcxText(message, capacity, position, ellipse ? "楕円範囲" : "指定なし"))
            return false;
    }
    return appendDcxText(message, capacity, position, "\n");
}

bool formatDcxMessage(const uint8_t *frame, char *message, size_t capacity, size_t &position)
{
    const uint32_t messageType = getBits(frame, 24, 2);
    const uint32_t country = getBits(frame, 26, 9);
    const uint32_t provider = getBits(frame, 35, 5);
    const uint32_t hazard = getBits(frame, 40, 7);
    const uint32_t severity = getBits(frame, 47, 2);
    const uint32_t library = getBits(frame, 66, 1);
    const uint32_t instruction = getBits(frame, 70, 10);
    static const char *const messageTypes[] = {"試験", "警報", "更新", "解除"};

    message[0] = '\0';
    position = 0;
    if (!appendDcxText(message, capacity, position, "みちびきDCX【") ||
        !appendDcxText(message, capacity, position, messageTypes[messageType]) ||
        !appendDcxText(message, capacity, position, "】\n種別:"))
        return false;
    const char *hazardText = dcxHazardText(hazard);
    if (hazardText) {
        if (!appendDcxText(message, capacity, position, hazardText))
            return false;
    } else if (!appendDcxText(message, capacity, position, "コード") ||
               !appendDcxNumber(message, capacity, position, static_cast<unsigned>(hazard))) {
        return false;
    }
    if (messageType != 0 && (!appendDcxText(message, capacity, position, "・") ||
                             !appendDcxText(message, capacity, position, dcxSeverityText(severity))))
        return false;
    if (!appendDcxText(message, capacity, position, "\n") ||
        !appendDcxTarget(frame, country, provider, message, capacity, position))
        return false;

    char instructionText[144];
    if (country != 111 || library != 1) {
        std::snprintf(instructionText, sizeof(instructionText), "行動指示は未対応です。");
    } else if (!formatDcxInstruction(instruction, instructionText, sizeof(instructionText))) {
        return false;
    }
    return instructionText[0] == '\0' || (appendDcxText(message, capacity, position, instructionText));
}

void publishFrame(const uint8_t *payload)
{
    if (payload[0] != 5 || payload[2] != 1) {
        ++nonL1sSfrbxMessages;
        return;
    }
    ++l1sSfrbxMessages;
    if (payload[4] != 8 && payload[4] != 9) {
        ++l1sWordCountErrors;
        return;
    }
    const uint8_t svId = payload[1];

    uint8_t frame[32];
    for (uint8_t word = 0; word < 8; ++word) {
        const uint8_t offset = 8 + word * 4;
        frame[word * 4] = payload[offset + 3];
        frame[word * 4 + 1] = payload[offset + 2];
        frame[word * 4 + 2] = payload[offset + 1];
        frame[word * 4 + 3] = payload[offset];
    }

    const uint8_t preamble = frame[0];
    if (preamble != L1S_PREAMBLES[0] && preamble != L1S_PREAMBLES[1] && preamble != L1S_PREAMBLES[2]) {
        ++l1sPreambleRejects;
        return;
    }
    ++l1sPreambleMatches;
    if (crc24q(frame) != getBits(frame, 226, 24)) {
        ++l1sCrcRejects;
        return;
    }
    ++l1sCrcMatches;
    const uint32_t now = Time::getMillis();
    lastValidL1SMs = now;
    hasValidL1S = true;
    const uint8_t messageType = static_cast<uint8_t>(getBits(frame, 8, 6));
    if (svId > 0 && svId <= MAX_MICHIBIKI_SV_ID) {
        ++l1sFramesBySatellite[svId - 1];
        lastL1SBySatelliteMs[svId - 1].store(now);
        lastMessageTypeBySatellite[svId - 1].store(messageType);
        hasL1SBySatellite[svId - 1].store(true);
    }
    const bool satelliteRegistered = registerMichibikiSatellite(svId);
    if (satelliteRegistered)
        recordMichibikiSatelliteReception(svId);

    lastCaptureSvId.store(svId);
    lastCaptureMs.store(Time::getMillis());
    hasCapturedSatellite.store(true);

    if (messageType == 43) {
        ++l1sMt43Messages;
    } else if (messageType == 44) {
        ++l1sMt44Messages;
    } else if (messageType == 63) {
        ++l1sMt63Messages;
        return;
    } else {
        ++l1sOtherMessageTypes;
        return;
    }
    lastMt43Or44Ms = now;
    hasMt43Or44 = true;

    frame[31] &= 0xC0;
    if (seenRecently(frameHash(frame)) || channelIndex == NO_CHANNEL || !router || !service)
        return;

    char message[meshtastic_Constants_DATA_PAYLOAD_LEN];
    size_t position = 0;
    if (messageType == 43) {
        if (!formatDcrMessage(frame, message, sizeof(message), position))
            return;
    } else {
        if (!formatDcxMessage(frame, message, sizeof(message), position))
            return;
    }

    meshtastic_MeshPacket *packet = router->allocForSending();
    if (!packet)
        return;
    packet->channel = channelIndex;
    packet->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
    packet->decoded.payload.size = static_cast<uint16_t>(position);
    std::memcpy(packet->decoded.payload.bytes, message, position);
    packet->decoded.want_response = false;
    if (satelliteRegistered) {
        packet->from = MICHIBIKI_NODE_NUM_BASE + svId;
    }

#if HAS_SCREEN || defined(MESHTASTIC_INCLUDE_NICHE_GRAPHICS)
    messageStore.tryAddFromPacket(*packet);
#endif
    service->sendToPhone(packet);
    if (externalNotificationModule)
        externalNotificationModule->startNotification();
}

void processUbxMessage()
{
    if (parser.messageClass == 0x02 && parser.messageId == 0x13) {
        ++sfrbxMessages;
        if (parser.length >= 8 && parser.length == 8 + static_cast<uint16_t>(parser.payload[4]) * 4)
            publishFrame(parser.payload);
        else
            ++sfrbxLengthErrors;
    } else if (parser.messageClass == 0x01 && parser.messageId == 0x35 && parser.length >= 8) {
        const uint8_t numSvs = parser.payload[5];
        if (parser.length != 8 + static_cast<uint16_t>(numSvs) * 12)
            return;

        uint16_t satellites[MAX_MICHIBIKI_SV_ID] = {};
        uint8_t diagnosticCno[MAX_MICHIBIKI_SV_ID] = {};
        for (uint8_t i = 0; i < numSvs; ++i) {
            const uint8_t *entry = parser.payload + 8 + static_cast<uint16_t>(i) * 12;
            if (entry[0] != 5 || entry[1] == 0 || entry[1] > MAX_MICHIBIKI_SV_ID)
                continue;

            const uint8_t svId = entry[1];
            if (entry[2] > 0) {
                diagnosticCno[svId - 1] = entry[2];
                lastQzssCno[svId - 1].store(entry[2]);
                if (registerMichibikiSatellite(svId))
                    recordMichibikiSatelliteReception(svId);
            }
            if (entry[2] == 0)
                continue;

            const uint32_t flags = static_cast<uint32_t>(entry[8]) | (static_cast<uint32_t>(entry[9]) << 8) |
                                   (static_cast<uint32_t>(entry[10]) << 16) | (static_cast<uint32_t>(entry[11]) << 24);
            const uint8_t quality = flags & 0x07;
            if (quality < 2)
                continue;

            satellites[svId - 1] = static_cast<uint16_t>(svId) | (static_cast<uint16_t>(entry[2]) << 8);
        }

        for (uint8_t i = 0; i < MAX_MICHIBIKI_SV_ID; ++i) {
            qzssSatelliteData[i].store(satellites[i]);
            diagnosticQzssCno[i].store(diagnosticCno[i]);
        }
        lastSatelliteReportMs.store(Time::getMillis());
        hasSatelliteReport.store(true);
    }

}
} // namespace

bool setupChannel()
{
    loadSettings();
    if (channelFile.channels_count == 0)
        return false;

    for (ChannelIndex i = 0; i < channelFile.channels_count; ++i) {
        meshtastic_Channel &channel = channels.getByIndex(i);
        if (channel.has_settings && std::strcmp(channel.settings.name, CHANNEL_NAME) == 0) {
            channelIndex = i;
            if (channel.role == meshtastic_Channel_Role_DISABLED) {
                channel.role = meshtastic_Channel_Role_SECONDARY;
                channels.onConfigChanged();
                nodeDB->saveToDisk(SEGMENT_CHANNELS);
            }
            return true;
        }
    }

    for (ChannelIndex i = 0; i < channelFile.channels_count; ++i) {
        meshtastic_Channel &channel = channels.getByIndex(i);
        if (channel.role != meshtastic_Channel_Role_DISABLED ||
            (channel.has_settings && !isEmptyChannelSettings(channel.settings)))
            continue;

        channel = meshtastic_Channel_init_zero;
        channel.index = i;
        channel.role = meshtastic_Channel_Role_SECONDARY;
        channel.has_settings = true;
        std::strncpy(channel.settings.name, CHANNEL_NAME, sizeof(channel.settings.name) - 1);
        channel.settings.psk.size = 0;
        channels.onConfigChanged();
        channelIndex = i;
        nodeDB->saveToDisk(SEGMENT_CHANNELS);
        return true;
    }

    LOG_WARN("No unused channel for QZSS DCR messages");
    return false;
}

bool isEnabled()
{
    loadSettings();
    return enabled.load();
}

bool isUbloxM10Detected()
{
    return m10Detected.load();
}

bool getCapturedSatellite(uint8_t &svId, uint8_t *ageSeconds)
{
    const uint32_t lastFrameMs = lastCaptureMs.load();
    if (!hasCapturedSatellite.load() || Throttle::hasElapsed(lastFrameMs, CAPTURE_TIMEOUT_MS))
        return false;

    svId = lastCaptureSvId.load();
    if (ageSeconds)
        *ageSeconds = static_cast<uint8_t>((Time::getMillis() - lastFrameMs) / 1000);
    return true;
}

bool getSatelliteStatus(SatelliteStatus *satellites, uint8_t &count)
{
    count = 0;
    if (!satellites || !hasSatelliteReport.load() ||
        Throttle::hasElapsed(lastSatelliteReportMs.load(), 5000))
        return false;

    for (uint8_t i = 0; i < MAX_MICHIBIKI_SV_ID; ++i) {
        const uint32_t packed = qzssSatelliteData[i].load();
        if (!packed)
            continue;
        satellites[count++] = {static_cast<uint8_t>(packed), static_cast<uint8_t>(packed >> 8)};
    }
    return true;
}

bool getLastSatelliteCno(uint8_t svId, uint8_t &cno)
{
    if (svId == 0 || svId > MAX_MICHIBIKI_SV_ID)
        return false;

    cno = lastQzssCno[svId - 1].load();
    return cno != 0;
}

bool getSatelliteReceptionStatus(uint8_t svId, SatelliteReceptionStatus &status)
{
    if (svId == 0 || svId > MAX_MICHIBIKI_SV_ID)
        return false;

    const uint8_t index = svId - 1;
    status = {};
    status.cno = lastQzssCno[index].load();
    status.hasL1S = hasL1SBySatellite[index].load();
    if (status.hasL1S) {
        status.messageType = lastMessageTypeBySatellite[index].load();
        status.lastL1SAgeSeconds = (Time::getMillis() - lastL1SBySatelliteMs[index].load()) / 1000;
    }
    return true;
}

void logDiagnostics(const char *powerState)
{
    const uint32_t now = Time::getMillis();
    if (!Throttle::hasElapsed(lastDiagnosticLogMs, DIAGNOSTIC_LOG_INTERVAL_MS))
        return;

    LOG_INFO("QZSS L1S/30s: SFRBX=%lu badCK=%lu large=%lu badLen=%lu otherSig=%lu L1S=%lu badWords=%lu "
             "PAB=%lu/%lu CRC=%lu/%lu MT43=%lu MT44=%lu MT63=%lu otherMT=%lu",
             static_cast<unsigned long>(sfrbxMessages), static_cast<unsigned long>(sfrbxChecksumErrors),
             static_cast<unsigned long>(sfrbxOversizeMessages), static_cast<unsigned long>(sfrbxLengthErrors),
             static_cast<unsigned long>(nonL1sSfrbxMessages), static_cast<unsigned long>(l1sSfrbxMessages),
             static_cast<unsigned long>(l1sWordCountErrors), static_cast<unsigned long>(l1sPreambleMatches),
             static_cast<unsigned long>(l1sPreambleRejects), static_cast<unsigned long>(l1sCrcMatches),
             static_cast<unsigned long>(l1sCrcRejects), static_cast<unsigned long>(l1sMt43Messages),
             static_cast<unsigned long>(l1sMt44Messages), static_cast<unsigned long>(l1sMt63Messages),
             static_cast<unsigned long>(l1sOtherMessageTypes));

    char satelliteSummary[384] = {};
    size_t position = 0;
    if (hasSatelliteReport.load() && !Throttle::hasElapsed(lastSatelliteReportMs.load(), 5000)) {
        for (uint8_t i = 0; i < MAX_MICHIBIKI_SV_ID; ++i) {
            const uint8_t cno = diagnosticQzssCno[i].load();
            if (!cno)
                continue;
            const uint8_t svId = i + 1;
            const int length = std::snprintf(satelliteSummary + position, sizeof(satelliteSummary) - position,
                                             "%sSV%u C/N0=%u frames=%lu", position ? ", " : "",
                                             static_cast<unsigned>(192 + svId),
                                             static_cast<unsigned>(cno),
                                             static_cast<unsigned long>(l1sFramesBySatellite[svId - 1]));
            if (length < 0 || static_cast<size_t>(length) >= sizeof(satelliteSummary) - position)
                break;
            position += static_cast<size_t>(length);
        }
    }
    if (!position)
        std::strncpy(satelliteSummary, "NONE", sizeof(satelliteSummary) - 1);

    char lastL1SAge[16] = "never";
    char lastMtAge[16] = "never";
    if (hasValidL1S)
        std::snprintf(lastL1SAge, sizeof(lastL1SAge), "%lus", static_cast<unsigned long>((now - lastValidL1SMs) / 1000));
    if (hasMt43Or44)
        std::snprintf(lastMtAge, sizeof(lastMtAge), "%lus", static_cast<unsigned long>((now - lastMt43Or44Ms) / 1000));
    LOG_INFO("QZSS sats: %s; lastL1S=%s lastMT43/44=%s pwr=%s", satelliteSummary, lastL1SAge, lastMtAge, powerState);

    lastDiagnosticLogMs = now;
    sfrbxMessages = 0;
    sfrbxChecksumErrors = 0;
    sfrbxOversizeMessages = 0;
    sfrbxLengthErrors = 0;
    nonL1sSfrbxMessages = 0;
    l1sSfrbxMessages = 0;
    l1sWordCountErrors = 0;
    l1sPreambleRejects = 0;
    l1sPreambleMatches = 0;
    l1sCrcRejects = 0;
    l1sCrcMatches = 0;
    l1sOtherMessageTypes = 0;
    l1sMt43Messages = 0;
    l1sMt44Messages = 0;
    l1sMt63Messages = 0;
    std::memset(l1sFramesBySatellite, 0, sizeof(l1sFramesBySatellite));
}

void setUbloxM10Detected(bool detected)
{
    m10Detected.store(detected);
    if (!detected) {
        hasCapturedSatellite.store(false);
        hasSatelliteReport.store(false);
        for (auto &satellite : qzssSatelliteData)
            satellite.store(0);
        for (auto &cno : diagnosticQzssCno)
            cno.store(0);
    }
}

bool setEnabled(bool value)
{
    loadSettings();
    if (enabled.load() == value)
        return true;

    const bool previous = enabled.load();
    enabled.store(value);
    if (!saveSettings()) {
        enabled.store(previous);
        LOG_WARN("Failed to save QZSS L1S setting");
        return false;
    }
    hasCapturedSatellite.store(false);
    hasSatelliteReport.store(false);
    for (auto &satellite : qzssSatelliteData)
        satellite.store(0);
    for (auto &cno : diagnosticQzssCno)
        cno.store(0);
    parserResetPending.store(true);
    return true;
}

void reset()
{
    parser.reset();
    hasCapturedSatellite.store(false);
    hasSatelliteReport.store(false);
    for (auto &satellite : qzssSatelliteData)
        satellite.store(0);
    for (auto &cno : diagnosticQzssCno)
        cno.store(0);
}

bool consumeByte(uint8_t byte)
{
    if (parserResetPending.exchange(false))
        parser.reset();
    if (!isEnabled())
        return false;

    switch (parser.state) {
    case ParserState::Sync1:
        if (byte != 0xB5)
            return false;
        parser.state = ParserState::Sync2;
        return true;
    case ParserState::Sync2:
        if (byte == 0x62) {
            parser.state = ParserState::Class;
            parser.checksumA = 0;
            parser.checksumB = 0;
        } else if (byte != 0xB5) {
            parser.reset();
        }
        return true;
    case ParserState::Class:
        parser.messageClass = byte;
        parser.checksum(byte);
        parser.state = ParserState::Id;
        return true;
    case ParserState::Id:
        parser.messageId = byte;
        parser.checksum(byte);
        parser.state = ParserState::LengthLow;
        return true;
    case ParserState::LengthLow:
        parser.length = byte;
        parser.checksum(byte);
        parser.state = ParserState::LengthHigh;
        return true;
    case ParserState::LengthHigh:
        parser.length |= static_cast<uint16_t>(byte) << 8;
        parser.checksum(byte);
        parser.position = 0;
        parser.payloadTooLarge = parser.length > sizeof(parser.payload);
        parser.state = parser.length ? ParserState::Payload : ParserState::ChecksumA;
        return true;
    case ParserState::Payload:
        if (!parser.payloadTooLarge)
            parser.payload[parser.position] = byte;
        parser.checksum(byte);
        if (++parser.position == parser.length)
            parser.state = ParserState::ChecksumA;
        return true;
    case ParserState::ChecksumA:
        parser.receivedChecksumA = byte;
        parser.state = ParserState::ChecksumB;
        return true;
    case ParserState::ChecksumB:
        if (parser.payloadTooLarge && parser.messageClass == 0x02 && parser.messageId == 0x13)
            ++sfrbxOversizeMessages;
        else if (parser.receivedChecksumA != parser.checksumA || byte != parser.checksumB) {
            if (parser.messageClass == 0x02 && parser.messageId == 0x13)
                ++sfrbxChecksumErrors;
        } else
            processUbxMessage();
        parser.reset();
        return true;
    }
    parser.reset();
    return false;
}
} // namespace QzssDcr
#endif
