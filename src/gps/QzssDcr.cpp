#include "configuration.h"
#if defined(HAS_QZSS_DCR)
#if HAS_SCREEN || defined(MESHTASTIC_INCLUDE_NICHE_GRAPHICS)
#include "MessageStore.h"
#endif
#include "FSCommon.h"
#include "GPSLog.h"
#include "QzssDcr.h"
#include "QzssDcrCodeTables.h"
#include "QzssMunicipalityNames.h"
#include "SPILock.h"
#include "SafeFile.h"
#include "UptimeClock.h"
#include "gps/RTC.h"
#include "mesh/Channels.h"
#include "mesh/MeshService.h"
#include "mesh/NodeDB.h"
#include "mesh/Router.h"
#include "mesh/Throttle.h"
#include "mesh/TypeConversions.h"
#include "meshUtils.h"
#include "modules/ExternalNotificationModule.h"
#ifdef MESHTASTIC_ENCRYPTED_STORAGE
#include "security/EncryptedStorage.h"
#endif
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <time.h>

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
constexpr uint32_t MESSAGE_DEDUP_INTERVAL_MS = 60 * 60 * 1000;
constexpr uint8_t MAX_MICHIBIKI_SV_ID = MAX_QZSS_SATELLITES;
constexpr uint8_t QZSS_L1CA_SIG_ID = 0;
constexpr uint8_t QZSS_L1S_SIG_ID = 1;
constexpr uint8_t MAX_SIGNAL_ID = 16;
// Keep full MT frames for an hour; increase if the window can exceed this volume.
constexpr size_t RECENT_MESSAGE_CAPACITY = 128;

struct RecentMtFrame {
    uint64_t fingerprint;
    uint32_t receivedAtMs;
    bool used;
};

std::atomic<uint8_t> diagnosticQzssCno[MAX_MICHIBIKI_SV_ID] = {};
std::atomic<uint8_t> lastQzssCno[MAX_MICHIBIKI_SV_ID] = {};
std::atomic<uint32_t> lastL1SBySatelliteMs[MAX_MICHIBIKI_SV_ID] = {};
std::atomic<uint8_t> lastMessageTypeBySatellite[MAX_MICHIBIKI_SV_ID] = {};
std::atomic<bool> hasL1SBySatellite[MAX_MICHIBIKI_SV_ID] = {};
uint32_t lastNodeHeardUpdateMs[MAX_MICHIBIKI_SV_ID] = {};
bool hasNodeHeardUpdate[MAX_MICHIBIKI_SV_ID] = {};

uint8_t channelIndex = NO_CHANNEL;
RecentMtFrame recentMtFrames[RECENT_MESSAGE_CAPACITY] = {};
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
uint32_t nonQzssSfrbxMessages = 0;
uint32_t qzssSfrbxBySignalId[MAX_SIGNAL_ID] = {};
uint32_t qzssSfrbxUnknownSignalIds = 0;
uint32_t qzssSfrbxBySatelliteSignal[MAX_MICHIBIKI_SV_ID][2] = {};
uint32_t navSatReports = 0;
uint32_t navSatReportsWithQzss = 0;
uint32_t navSatQzssSatelliteObservations = 0;
uint32_t l1sWordCountErrors = 0;
uint32_t l1sPreambleRejects = 0;
uint32_t l1sPreambleMatches = 0;
uint32_t l1sCrcRejects = 0;
uint32_t l1sCrcMatches = 0;
uint32_t l1sMessageTypeCounts[64] = {};
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

uint64_t getBits64(const uint8_t *frame, uint16_t firstBit, uint8_t count)
{
    uint64_t value = 0;
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
    std::snprintf(satellite.short_name, sizeof(satellite.short_name), "Q%u", static_cast<unsigned>(svId));
    satellite.hw_model = meshtastic_HardwareModel_UNSET;
    satellite.role = meshtastic_Config_DeviceConfig_Role_CLIENT;
    if (existing && existing->channel == channelIndex && existing->hw_model == satellite.hw_model &&
        existing->role == satellite.role && !nodeInfoLiteIsLicensed(existing) &&
        std::strcmp(existing->short_name, satellite.short_name) == 0)
        return true;

    std::snprintf(satellite.long_name, sizeof(satellite.long_name), "Michibiki SV%u", static_cast<unsigned>(svId));
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

uint64_t mtFrameFingerprint(const uint8_t *frame)
{
    uint64_t hash = 14695981039346656037ULL;
    const auto addByte = [&hash](uint8_t byte) { hash = (hash ^ byte) * 1099511628211ULL; };
    // The preamble, trailing reserved bits and CRC do not identify a report.
    for (uint16_t bit = 8; bit < 220; ++bit)
        addByte(getBit(frame, bit));
    return hash;
}

bool isDuplicateMtFrame(const uint8_t *frame)
{
    const uint64_t fingerprint = mtFrameFingerprint(frame);
    const uint32_t now = Time::getMillis();
    size_t availableIndex = RECENT_MESSAGE_CAPACITY;
    size_t oldestIndex = RECENT_MESSAGE_CAPACITY;
    uint32_t oldestAge = 0;

    for (size_t i = 0; i < RECENT_MESSAGE_CAPACITY; ++i) {
        const RecentMtFrame &recent = recentMtFrames[i];
        if (!recent.used || Throttle::hasElapsed(recent.receivedAtMs, MESSAGE_DEDUP_INTERVAL_MS)) {
            if (availableIndex == RECENT_MESSAGE_CAPACITY)
                availableIndex = i;
            continue;
        }
        if (recent.fingerprint == fingerprint)
            return true;

        const uint32_t age = now - recent.receivedAtMs;
        if (oldestIndex == RECENT_MESSAGE_CAPACITY || age > oldestAge) {
            oldestIndex = i;
            oldestAge = age;
        }
    }

    if (availableIndex == RECENT_MESSAGE_CAPACITY)
        availableIndex = oldestIndex;
    recentMtFrames[availableIndex] = {fingerprint, now, true};
    return false;
}

const char *dcxHazardText(uint32_t code)
{
    switch (code) {
    case 1:
        return "危険物質等による空爆";
    case 2:
        return "危険物質等を用いた情報システム攻撃";
    case 3:
        return "核兵器による攻撃";
    case 4:
        return "生物災害";
    case 5:
        return "化学災害";
    case 6:
        return "爆発物災害";
    case 7:
        return "隕石衝突";
    case 8:
        return "ミサイル攻撃";
    case 9:
        return "核災害";
    case 10:
        return "原子力発電所事故";
    case 11:
        return "放射線災害";
    case 12:
        return "宇宙デブリ再突入";
    case 13:
        return "サイレン試験";
    case 14:
        return "酸性雨";
    case 15:
        return "大気汚染";
    case 16:
        return "飲料水汚染";
    case 17:
        return "ガス漏れ";
    case 18:
        return "海洋汚染";
    case 19:
        return "騒音";
    case 20:
        return "害虫大量発生";
    case 21:
        return "河川汚染";
    case 22:
        return "浮遊粉じん";
    case 23:
        return "紫外線";
    case 24:
        return "大規模火災";
    case 25:
        return "消防出動";
    case 26:
        return "火災ガス";
    case 27:
        return "森林火災";
    case 28:
        return "煙・有害ガス";
    case 29:
        return "悪臭";
    case 30:
        return "火災危険";
    case 31:
        return "建物・工場火災";
    case 32:
        return "降灰";
    case 33:
        return "雪崩";
    case 34:
        return "地割れ・陥没";
    case 35:
        return "土石流";
    case 36:
        return "地震";
    case 37:
        return "地磁気・太陽嵐";
    case 38:
        return "氷河雪崩";
    case 39:
        return "地滑り";
    case 40:
        return "溶岩流";
    case 41:
        return "火砕流";
    case 42:
        return "吹きだまり";
    case 43:
        return "潮位・高波";
    case 44:
        return "津波";
    case 45:
        return "火山泥流";
    case 46:
        return "火山噴火";
    case 47:
        return "高潮・風浪";
    case 48:
        return "家畜伝染病";
    case 49:
        return "食品安全警報";
    case 50:
        return "健康被害";
    case 51:
        return "パンデミック";
    case 52:
        return "害虫被害";
    case 53:
        return "感染リスク";
    case 54:
        return "建物崩壊";
    case 55:
        return "緊急通報停止";
    case 56:
        return "ガス供給停止";
    case 57:
        return "情報システム停止";
    case 58:
        return "停電";
    case 59:
        return "下水未処理放出";
    case 60:
        return "電話回線停止";
    case 61:
        return "路面凍結";
    case 62:
        return "沿岸洪水";
    case 63:
        return "寒波";
    case 64:
        return "デレーチョ（直線状風）";
    case 65:
        return "干ばつ";
    case 66:
        return "砂嵐";
    case 67:
        return "流氷・氷山";
    case 68:
        return "洪水";
    case 69:
        return "霧";
    case 70:
        return "雹";
    case 71:
        return "熱波";
    case 72:
        return "雷";
    case 73:
        return "花粉";
    case 74:
        return "降雨";
    case 75:
        return "暴風雪";
    case 76:
        return "降雪";
    case 77:
        return "暴風雨・雷雨";
    case 78:
        return "雪解け";
    case 79:
        return "竜巻";
    case 80:
        return "熱帯低気圧・ハリケーン";
    case 81:
        return "風冷え・霜";
    case 82:
        return "台風";
    case 83:
        return "ダム決壊";
    case 84:
        return "堤防決壊";
    case 85:
        return "不発弾処理";
    case 86:
        return "工場事故";
    case 87:
        return "鉱山災害";
    case 88:
        return "爆弾・弾薬発見";
    case 89:
        return "デモ";
    case 90:
        return "危険物事故";
    case 91:
        return "生命の危険";
    case 92:
        return "大規模イベント";
    case 93:
        return "行方不明・誘拐";
    case 94:
        return "爆発の危険";
    case 95:
        return "安全上の警報";
    case 96:
        return "未確認飛行物体";
    case 97:
        return "未確認動物";
    case 98:
        return "化学攻撃";
    case 99:
        return "ゲリラ攻撃";
    case 100:
        return "ハイジャック";
    case 101:
        return "銃撃・武器の危険";
    case 102:
        return "特殊部隊攻撃";
    case 103:
        return "テロ";
    case 104:
        return "航空機事故";
    case 105:
        return "橋梁崩落";
    case 106:
        return "危険物事故";
    case 107:
        return "内陸水路事故";
    case 108:
        return "海上保安事故";
    case 109:
        return "油流出";
    case 110:
        return "道路交通事故";
    case 111:
        return "鉄道事故";
    case 112:
        return "トンネル事故";
    case 113:
        return "試験情報";
    case 0:
        return "未使用";
    default:
        return code <= 127 ? "予約" : nullptr;
    }
}

const char *dcxCountryText(uint32_t code)
{
    switch (code) {
    case 10:
        return "オーストラリア";
    case 71:
        return "フィジー";
    case 111:
        return "日本";
    case 219:
        return "タイ";
    case 500:
        return "欧州連合機関";
    case 501:
        return "国連機関";
    case 502:
        return "国際";
    default:
        return code <= 248 ? "国・地域名未登録" : "予約";
    }
}

const char *dcxProviderText(uint32_t country, uint32_t provider)
{
    static const char *const japan[] = {"未使用", "マルチメディア振興センター", "消防庁", "関係省庁", "自治体"};
    static const char *const australia[] = {"未使用", "国家緊急事態管理庁", "気象局", "気候サービス", "地球科学機構", "連邦科学産業研究機構",
                                            "統計局", "ニューサウスウェールズ州復興庁", "同州緊急サービス", "同州地方消防局",
                                            "合同津波警報センター", "洪水情報センター", "放送協会"};
    static const char *const fiji[] = {"未使用", "国家災害管理局", "気象局", "水道公社水文部門", "鉱物資源局", "放送協会"};
    static const char *const thailand[] = {"未使用", "防災軽減局", "気象局", "国家災害警報センター", "鉱物資源局", "海軍水路部",
                                           "水資源局", "灌漑局", "公害管理局", "地理情報・宇宙技術開発機関", "発電公社",
                                           "森林局", "国立公園・野生生物・植物保全局", "水危機対策センター"};
    const char *const *names = nullptr;
    size_t count = 0;
    switch (country) {
    case 10:
        names = australia;
        count = sizeof(australia) / sizeof(australia[0]);
        break;
    case 71:
        names = fiji;
        count = sizeof(fiji) / sizeof(fiji[0]);
        break;
    case 111:
        names = japan;
        count = sizeof(japan) / sizeof(japan[0]);
        break;
    case 219:
        names = thailand;
        count = sizeof(thailand) / sizeof(thailand[0]);
        break;
    default:
        return "国・地域別定義";
    }
    return provider < count ? names[provider] : "予約";
}

const char *dcxInternationalInstructionA(uint32_t code)
{
    static const char *const instructions[] = {
        "指示なし", "危険区域から直ちに退避。続報を確認。", "危険区域から直ちに指定避難先へ。続報を確認。",
        "直ちに建物内へ。屋内避難を続け、続報を確認。", "地下・低層階へ避難", "山地から下山",
        "屋内へ・車は路肩停車", "伏せて頭と首を保護", "避難準備", "非常物資を準備", "窓ガラスから離れる",
        "節電", "節水", "水道水は煮沸", "人との距離を確保・手洗い", "水道水を飲まない", "ガス漏れと火気に注意",
        "外出・車の使用を避ける", "不審物やがれきに触れない", "煙の充満した部屋に入らない", "地下室に入らない",
        "ペット・家畜を屋内へ", "死んだ動物に触れない", "運転を避ける", "雷の間は金属・水を避ける",
        "屋根直下や大空間を避ける", "屋外・高所・電線から離れる", "危険区域を避ける", "不要不急の外出を避ける",
        "試験情報・対応不要", "従来の警報を更新", "警報基準を下回った状態"};
    return code < sizeof(instructions) / sizeof(instructions[0]) ? instructions[code] : "未定義の行動指示";
}

const char *dcxInternationalInstructionB(uint32_t code)
{
    static const char *const instructions[] = {
        "指示なし", "気象機関・自治体に確認", "公式な情報提供先を確認", "要配慮者は外出を控える", "救助活動中・移動を控える",
        "要配慮者を支援", "警察・消防の発表を確認", "公式発表を継続確認", "避難支援を要請", "緊急時以外の通話を控える",
        "極端な悪天候・厳重警戒", "悪天候・事前の備え", "悪天候・最新情報を確認", "洪水に備える", "浸水場所に近づかない",
        "頑丈な建物内に避難", "高温・熱中症に注意", "山火事警戒・火気禁止", "火災を予防", "津波から高所へ避難",
        "海に出ず海岸から離れる", "影響区域から高所へ避難", "地震時は身を守り揺れ後に避難", "危険物質と風下を避ける",
        "換気を止め窓を閉める", "ヨウ素剤を準備・まだ服用しない", "指示に従いヨウ素剤を服用", "節水",
        "避難できない場合は屋内退避", "未定義の指示", "未定義の指示", "従来の警報を更新"};
    return code < sizeof(instructions) / sizeof(instructions[0]) ? instructions[code] : "未定義の行動指示";
}

const char *dcxSecondEllipseInstruction(uint32_t code)
{
    static const char *const instructions[32] = {
        "指示なし", "避難準備", "非常物資を準備", "洪水に備える", "ヨウ素剤を準備・まだ服用しない", "スマートフォンを充電",
        "エレベーターを使わない", "危険区域を避ける", "運転を避ける", "救助活動中・移動を控える", "気象機関・自治体に確認",
        "公式な情報提供先を確認", "要配慮者は外出を控える", "要配慮者を支援", "警察・消防の発表を確認", "公式発表を継続確認",
        "緊急時以外の通話を控える", "未定義の指示", "未定義の指示", "未定義の指示", "未定義の指示", "未定義の指示",
        "未定義の指示", "未定義の指示", "未定義の指示", "未定義の指示", "未定義の指示", "未定義の指示",
        "未定義の指示", "未定義の指示", "試験情報・対応不要", "警報基準を下回った状態"};
    return instructions[code & 0x1F];
}

template <size_t N> const char *dcxDetailTableText(const char *const (&table)[N], uint32_t code)
{
    return code < N ? table[code] : "未定義コード";
}

const char *dcxHazardDetailText(uint8_t field, uint32_t code)
{
    static const char *const d1[] = {"マグニチュード1.0-1.9", "マグニチュード2.0-2.9", "マグニチュード3.0-3.9",
                                     "マグニチュード4.0-4.9", "マグニチュード5.0-5.9", "マグニチュード6.0-6.9",
                                     "マグニチュード7.0-7.9", "マグニチュード8.0-8.9", "マグニチュード9.0以上"};
    static const char *const d2[] = {"震度2", "震度3", "震度4", "震度5弱", "震度5強", "震度6弱", "震度6強", "震度7"};
    static const char *const d3[] = {"0°", "22.5°", "45°", "67.5°", "90°", "112.5°", "135°", "157.5°",
                                     "180°", "202.5°", "225°", "247.5°", "270°", "292.5°", "315°", "337.5°"};
    static const char *const d4[] = {"0.25倍", "0.5倍", "0.75倍", "1倍", "2倍", "3倍", "5倍", "10倍",
                                     "20倍", "30倍", "40倍", "50倍", "70倍", "100倍", "150倍", "200倍"};
    static const char *const d5[] = {"0.5m以下", "0.5-1.0m", "1.0-1.5m", "1.5-2.0m", "2.0-3.0m", "3.0-5.0m",
                                     "5.0-10.0m", "10.0m超"};
    static const char *const d6[] = {"-30°C以下", "-30〜-25°C", "-25〜-20°C", "-20〜-15°C", "-15〜-10°C", "-10〜-5°C",
                                     "-5〜0°C", "0〜5°C", "5〜10°C", "10〜15°C", "15〜20°C", "20〜25°C", "25〜30°C",
                                     "30〜35°C", "35〜45°C", "45°C超"};
    static const char *const d7[] = {"ハリケーン1級(120-150km/h)", "ハリケーン2級(150-180km/h)", "ハリケーン3級(180-210km/h)",
                                     "ハリケーン4級(210-250km/h)", "ハリケーン5級(250km/h以上)"};
    static const char *const d8[] = {"風力0・静穏(<1km/h)", "風力1・至軽風(1-5km/h)", "風力2・軽風(6-11km/h)",
                                     "風力3・軟風(12-19km/h)", "風力4・和風(20-30km/h)", "風力5・疾風(31-39km/h)",
                                     "風力6・雄風(40-50km/h)", "風力7・強風(51-61km/h)", "風力8・疾強風(62-74km/h)",
                                     "風力9・大強風(75-88km/h)", "風力10・全強風(89-102km/h)", "風力11・暴風(103-117km/h)",
                                     "風力12・台風(118km/h超)"};
    static const char *const d9[] = {"2.5mm/h以下", "2.5-7.5mm/h", "7.5-10mm/h", "10-20mm/h", "20-30mm/h", "30-50mm/h",
                                     "50-80mm/h", "80mm/h超"};
    static const char *const d10[] = {"被害分類1(規模1・強度1)", "被害分類2(規模1・強度2)", "被害分類3(規模1・強度3)",
                                      "被害分類4(規模2・強度1)", "被害分類5(規模2・強度2)", "被害分類5(規模3・強度3)"};
    static const char *const d11[] = {"竜巻の脅威なし", "非常に低い(2-4%)", "低い(5-14%)", "中程度(15-29%)", "高い(30-44%)",
                                      "極めて高い(45%以上)"};
    static const char *const d12[] = {"H0・硬い雹(約5mm)", "H1・植物に軽微な被害(5-15mm)", "H2・農作物に被害(10-20mm)",
                                      "H3・窓や構造物に被害(20-30mm)", "H4・広範囲の窓・車両に被害(25-40mm)",
                                      "H5・ガラス破損・負傷のおそれ(30-50mm)", "H6・航空機や壁面に被害(40-60mm)",
                                      "H7・屋根損傷・重傷のおそれ(50-75mm)", "H8・航空機に重大な被害(60-90mm)",
                                      "H9・甚大な構造被害(75-100mm)", "H10・甚大な構造被害(100mm超)"};
    static const char *const d13[] = {"濃霧・視程20m未満", "濃い霧・20-200m", "中程度の霧・200-500m", "薄い霧・500-1000m",
                                      "もや・1-2km", "煙霧・2-4km", "薄い煙霧・4-10km", "快晴・10-20km",
                                      "非常に良好・20-50km", "極めて良好・50km超"};
    static const char *const d14[] = {"20cm以下/日", "20-40cm/日", "40-60cm/日", "60-80cm/日", "80-100cm/日", "100-120cm/日",
                                      "120-140cm/日", "140-160cm/日", "160-180cm/日", "180-200cm/日", "200-220cm/日",
                                      "220-240cm/日", "240-260cm/日", "260-280cm/日", "280-300cm/日", "300-320cm/日",
                                      "320-340cm/日", "340-360cm/日", "360-380cm/日", "380-400cm/日", "400-420cm/日",
                                      "420-440cm/日", "440-460cm/日", "460-480cm/日", "480-500cm/日", "500-520cm/日",
                                      "520-540cm/日", "540-560cm/日", "560-580cm/日", "580-600cm/日", "600cm超/日"};
    static const char *const d15[] = {"小規模浸水・被害軽微", "中規模浸水・一部避難", "大規模浸水・広域避難", "記録的な洪水"};
    static const char *const d16[] = {"雷雨なし", "孤立した雷雨", "散発的な雷雨", "広範囲の雷雨", "多数の雷雨", "雨を伴わない雷"};
    static const char *const d17[] = {"霧レベル1/5・薄い霧", "霧レベル2/5・やや濃い", "霧レベル3/5・交通に支障",
                                      "霧レベル4/5・交通規制", "霧レベル5/5・濃霧"};
    static const char *const d18[] = {"D1・中程度の干ばつ", "D2・深刻な干ばつ", "D3・極端な干ばつ", "D4・例外的な干ばつ"};
    static const char *const d19[] = {"雪崩危険度1・低", "雪崩危険度2・中", "雪崩危険度3・かなり高い", "雪崩危険度4・高い",
                                      "雪崩危険度5・非常に高い"};
    static const char *const d20[] = {"降灰1mm未満", "降灰1-5mm", "降灰5-100mm", "降灰100-300mm", "降灰300mm超"};
    static const char *const d21[] = {"G1・地磁気嵐/弱", "G2・中程度", "G3・強い", "G4・非常に強い", "G5・極端に強い"};
    static const char *const d22[] = {"テロ脅威・非常に低い", "テロ脅威・低い", "テロ脅威・中程度", "テロ脅威・高い", "テロ脅威・危機的"};
    static const char *const d23[] = {"火災危険度1/5・低い", "火災危険度2/5・中程度", "火災危険度3/5・かなり高い",
                                      "火災危険度4/5・高い", "火災危険度5/5・非常に高い"};
    static const char *const d24[] = {"水質・非常に良好", "水質・良好", "水質・不良", "水質・非常に不良", "飲用可能", "飲用不可"};
    static const char *const d25[] = {"紫外線指数0-2・低い", "紫外線指数3/11・中程度", "紫外線指数4/11・中程度",
                                      "紫外線指数5/11・高い", "紫外線指数6/11・高い", "紫外線指数7/11・高い",
                                      "紫外線指数8/11・非常に高い", "紫外線指数9/11・非常に高い", "紫外線指数10/11・極端",
                                      "紫外線指数11/11・極端"};
    static const char *const d26[] = {"0-9人", "10-20人", "21-50人", "51-70人", "71-100人", "101-125人", "126-150人",
                                      "151-175人", "176-200人", "201-250人", "251-300人", "301-350人", "351-400人", "401-450人",
                                      "451-500人", "501-750人", "751-1000人", "1000人超", "2000人超", "3000人超", "5000人超"};
    static const char *const d27[] = {"40-45dB", "45-50dB", "50-60dB", "60-70dB", "70-80dB・大きい", "80-90dB・非常に大きい",
                                      "90-100dB・非常に大きい", "100-110dB・非常に大きい", "110-120dB・極めて大きい",
                                      "120-130dB・極めて大きい", "130-140dB・痛みを感じる", "140dB超・痛みを感じる"};
    static const char *const d28[] = {"大気質0-50・良好", "大気質51-100・普通", "大気質101-150・敏感な人に不健康",
                                      "大気質151-200・不健康", "大気質201-300・非常に不健康", "大気質301-500・危険"};
    static const char *const d29[] = {"30分未満", "30-45分", "45分-1時間", "1-1.5時間", "1.5-2時間", "2-3時間", "3-4時間",
                                      "4-5時間", "5-10時間", "10-24時間", "1-2日", "2-7日", "7日以上"};
    static const char *const d30[] = {"不明", "国際原子力事象評価尺度0・安全上の影響なし", "同尺度1・逸脱", "同尺度2・異常事象",
                                      "同尺度3・重大な異常事象", "同尺度4・局所的影響を伴う事故", "同尺度5・広範な影響を伴う事故",
                                      "同尺度6・大事故", "同尺度7・深刻な事故"};
    static const char *const d31[] = {"爆発物", "可燃性ガス", "可燃性エアゾール", "支燃性ガス", "高圧ガス", "可燃性液体", "可燃性固体",
                                      "自己反応性物質", "自然発火性液体", "自然発火性固体", "自己発熱性物質", "水反応性・可燃性ガス発生",
                                      "酸化性液体", "酸化性固体", "有機過酸化物", "金属腐食性物質"};
    static const char *const d32[] = {"バイオセーフティーレベル1", "バイオセーフティーレベル2", "バイオセーフティーレベル3",
                                      "バイオセーフティーレベル4"};
    static const char *const d33[] = {"生物病原体", "生物毒素", "血液・血液製剤", "環境試料"};
    static const char *const d34[] = {"PE1・大量爆発のおそれ", "PE2・飛散物の重大な危険", "PE3・火災・爆風・飛散の危険",
                                      "PE4・局所的な火災・爆発のおそれ"};
    static const char *const d35[] = {"炭疽", "ヒト鳥インフルエンザ", "ボツリヌス症", "ブルセラ症", "カンピロバクター症", "チクングニア熱",
                                      "クラミジア感染症", "コレラ", "新型コロナウイルス感染症", "変異型クロイツフェルト・ヤコブ病", "クリプトスポリジウム症",
                                      "デング熱", "ジフテリア", "エキノコックス症", "ジアルジア症", "淋菌感染症", "A型肝炎", "B型肝炎", "C型肝炎",
                                      "HIV感染症・後天性免疫不全症候群", "インフルエンザ菌b型感染症", "インフルエンザ", "侵襲性髄膜炎菌感染症", "侵襲性肺炎球菌感染症",
                                      "レジオネラ症", "レプトスピラ症", "リステリア症", "ライム神経ボレリア症", "マラリア", "麻疹", "侵襲性髄膜炎菌感染症",
                                      "流行性耳下腺炎", "百日咳", "ペスト", "侵襲性肺炎球菌感染症", "ポリオ", "Q熱", "狂犬病", "風疹", "先天性風疹",
                                      "サルモネラ症", "重症急性呼吸器症候群", "志賀毒素産生性大腸菌感染症", "細菌性赤痢", "天然痘", "梅毒", "先天性梅毒", "破傷風",
                                      "ダニ媒介性脳炎", "先天性トキソプラズマ症", "旋毛虫症", "結核", "野兎病", "腸チフス・パラチフス", "ウイルス性出血熱",
                                      "ウエストナイル熱", "黄熱", "エルシニア症", "ジカウイルス感染症", "先天性ジカウイルス感染症", "院内感染症", "薬剤耐性菌感染症", "原因不明の感染症"};
    static const char *const d36[] = {"台風規模1・強度1", "台風規模1・強度2", "台風規模1・強度3", "台風規模2・強度1", "台風規模2・強度2"};
    switch (field) {
    case 1: return dcxDetailTableText(d1, code);
    case 2: return dcxDetailTableText(d2, code);
    case 3: return dcxDetailTableText(d3, code);
    case 4: return dcxDetailTableText(d4, code);
    case 5: return dcxDetailTableText(d5, code);
    case 6: return dcxDetailTableText(d6, code);
    case 7: return dcxDetailTableText(d7, code);
    case 8: return dcxDetailTableText(d8, code);
    case 9: return dcxDetailTableText(d9, code);
    case 10: return dcxDetailTableText(d10, code);
    case 11: return dcxDetailTableText(d11, code);
    case 12: return dcxDetailTableText(d12, code);
    case 13: return dcxDetailTableText(d13, code);
    case 14: return dcxDetailTableText(d14, code);
    case 15: return dcxDetailTableText(d15, code);
    case 16: return dcxDetailTableText(d16, code);
    case 17: return dcxDetailTableText(d17, code);
    case 18: return dcxDetailTableText(d18, code);
    case 19: return dcxDetailTableText(d19, code);
    case 20: return dcxDetailTableText(d20, code);
    case 21: return dcxDetailTableText(d21, code);
    case 22: return dcxDetailTableText(d22, code);
    case 23: return dcxDetailTableText(d23, code);
    case 24: return dcxDetailTableText(d24, code);
    case 25: return dcxDetailTableText(d25, code);
    case 26: return dcxDetailTableText(d26, code);
    case 27: return dcxDetailTableText(d27, code);
    case 28: return dcxDetailTableText(d28, code);
    case 29: return dcxDetailTableText(d29, code);
    case 30: return dcxDetailTableText(d30, code);
    case 31: return dcxDetailTableText(d31, code);
    case 32: return dcxDetailTableText(d32, code);
    case 33: return dcxDetailTableText(d33, code);
    case 34: return dcxDetailTableText(d34, code);
    case 35: return dcxDetailTableText(d35, code);
    case 36: return dcxDetailTableText(d36, code);
    default: return "未対応項目";
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
        return "南海トラフ地震情報";
    case 5:
        return "津波情報";
    case 6:
        return "北西太平洋津波情報";
    case 8:
        return "火山情報";
    case 9:
        return "降灰情報";
    case 10:
        return "気象情報";
    case 11:
        return "洪水情報";
    case 12:
        return "台風情報";
    case 14:
        return "海上警報";
    default:
        return "災危通報（種別不明）";
    }
}

const char *dcrInformationTypeText(uint32_t hazard, uint32_t infoType)
{
    if (hazard == 6)
        return infoType == 0 ? "発表" : "仕様外";
    if (hazard == 14)
        return infoType == 0 ? "発表" : infoType == 1 ? "訂正" : "仕様外";
    if (hazard >= 1 && hazard <= 5)
        return infoType == 0 ? "発表" : infoType == 2 ? "取消" : "仕様外";
    if (hazard == 8 || (hazard >= 9 && hazard <= 12))
        return infoType == 0 ? "発表" : infoType == 1 ? "訂正" : infoType == 2 ? "取消" : "仕様外";
    return infoType < 3 ? (infoType == 0 ? "発表" : infoType == 1 ? "訂正" : "取消") : "未定義";
}

constexpr uint8_t MAX_DECODED_QZSS_MESSAGES = 16;

struct DecodedQzssMessages {
    char text[MAX_DECODED_QZSS_MESSAGES][meshtastic_Constants_DATA_PAYLOAD_LEN];
    uint16_t length[MAX_DECODED_QZSS_MESSAGES];
    uint8_t count;
};

bool appendDecodedQzssLine(DecodedQzssMessages &messages, const char *line)
{
    constexpr size_t capacity = meshtastic_Constants_DATA_PAYLOAD_LEN;
    static constexpr char continuation[] = "（続き）/";
    size_t index = messages.count - 1;
    const size_t length = std::strlen(line);
    if (messages.length[index] + length + 1 < capacity) {
        size_t position = messages.length[index];
        std::memcpy(messages.text[index] + position, line, length);
        position += length;
        messages.text[index][position++] = '/';
        messages.text[index][position] = '\0';
        messages.length[index] = static_cast<uint16_t>(position);
        return true;
    }

    size_t offset = 0;
    while (offset < length) {
        if (messages.count >= MAX_DECODED_QZSS_MESSAGES)
            return false;
        index = messages.count++;
        size_t position = sizeof(continuation) - 1;
        std::memcpy(messages.text[index], continuation, position);
        const size_t available = capacity - position - 1;
        const size_t remaining = length - offset;
        const bool completesLine = remaining < available;
        size_t count = completesLine ? remaining : available;
        if (!completesLine) {
            if (count == remaining)
                --count;
            while (count > 0 && (static_cast<uint8_t>(line[offset + count]) & 0xC0) == 0x80)
                --count;
            if (!count)
                return false;
        }
        std::memcpy(messages.text[index] + position, line + offset, count);
        position += count;
        offset += count;
        if (completesLine)
            messages.text[index][position++] = '/';
        messages.text[index][position] = '\0';
        messages.length[index] = static_cast<uint16_t>(position);
    }
    return true;
}

size_t formatUnsignedDecimal(char *output, size_t capacity, uint64_t value)
{
    char reversed[20];
    size_t length = 0;
    do {
        if (length == sizeof(reversed))
            return 0;
        reversed[length++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value);
    if (length >= capacity)
        return 0;
    for (size_t i = 0; i < length; ++i)
        output[i] = reversed[length - i - 1];
    output[length] = '\0';
    return length;
}

bool appendDecodedQzssNumber(DecodedQzssMessages &messages, const char *name, uint64_t value)
{
    char line[80];
    const size_t nameLength = std::strlen(name);
    if (nameLength + 1 >= sizeof(line))
        return false;
    std::memcpy(line, name, nameLength);
    line[nameLength] = '=';
    if (!formatUnsignedDecimal(line + nameLength + 1, sizeof(line) - nameLength - 1, value))
        return false;
    return appendDecodedQzssLine(messages, line);
}

bool appendDecodedQzssValue(DecodedQzssMessages &messages, const char *name, uint64_t value, const char *meaning)
{
    char line[meshtastic_Constants_DATA_PAYLOAD_LEN];
    char valueText[21];
    const size_t nameLength = std::strlen(name);
    const size_t valueLength = formatUnsignedDecimal(valueText, sizeof(valueText), value);
    if (nameLength + 1 >= sizeof(line) || !valueLength)
        return false;
    std::memcpy(line, name, nameLength);
    line[nameLength] = '=';
    size_t position = nameLength + 1;
    if (meaning) {
        const size_t meaningLength = std::strlen(meaning);
        if (position + meaningLength + valueLength + 3 > sizeof(line))
            return false;
        std::memcpy(line + position, meaning, meaningLength);
        position += meaningLength;
        line[position++] = '(';
    } else if (position + valueLength >= sizeof(line)) {
        return false;
    }
    std::memcpy(line + position, valueText, valueLength);
    position += valueLength;
    if (meaning)
        line[position++] = ')';
    line[position] = '\0';
    return appendDecodedQzssLine(messages, line);
}

bool appendDecodedQzssFixed(DecodedQzssMessages &messages, const char *name, int32_t scaledValue, uint8_t decimals,
                            const char *unit)
{
    static constexpr uint32_t scales[] = {1, 10, 100, 1000, 10000, 100000, 1000000};
    if (decimals >= sizeof(scales) / sizeof(scales[0]))
        return false;
    const int64_t signedValue = scaledValue;
    const uint64_t magnitude = signedValue < 0 ? static_cast<uint64_t>(-signedValue) : static_cast<uint64_t>(signedValue);
    const uint32_t scale = scales[decimals];
    char line[96];
    const size_t nameLength = std::strlen(name);
    if (nameLength + 1 >= sizeof(line))
        return false;
    std::memcpy(line, name, nameLength);
    size_t position = nameLength;
    line[position++] = '=';
    if (signedValue < 0) {
        if (position >= sizeof(line))
            return false;
        line[position++] = '-';
    }
    const size_t wholeLength = formatUnsignedDecimal(line + position, sizeof(line) - position, magnitude / scale);
    if (!wholeLength)
        return false;
    position += wholeLength;
    if (decimals) {
        if (position + decimals + 1 >= sizeof(line))
            return false;
        line[position++] = '.';
        uint32_t fraction = static_cast<uint32_t>(magnitude % scale);
        uint32_t divisor = scale / 10;
        for (uint8_t i = 0; i < decimals; ++i) {
            line[position++] = static_cast<char>('0' + fraction / divisor);
            fraction %= divisor;
            divisor /= 10;
        }
    }
    const size_t unitLength = std::strlen(unit);
    if (position + unitLength >= sizeof(line))
        return false;
    std::memcpy(line + position, unit, unitLength);
    position += unitLength;
    line[position] = '\0';
    return appendDecodedQzssLine(messages, line);
}

bool appendDecodedQzssHex(DecodedQzssMessages &messages, const char *name, uint32_t value, uint8_t digits)
{
    char line[48];
    static constexpr char hex[] = "0123456789ABCDEF";
    const size_t nameLength = std::strlen(name);
    size_t actualDigits = 1;
    for (uint32_t remaining = value; remaining >= 16; remaining >>= 4)
        ++actualDigits;
    const size_t hexLength = actualDigits > digits ? actualDigits : digits;
    if (nameLength + hexLength + 4 >= sizeof(line))
        return false;
    std::memcpy(line, name, nameLength);
    size_t position = nameLength;
    line[position++] = '=';
    line[position++] = '0';
    line[position++] = 'x';
    for (size_t i = 0; i < hexLength; ++i) {
        const size_t shift = (hexLength - i - 1) * 4;
        line[position++] = shift < 32 ? hex[(value >> shift) & 0xF] : '0';
    }
    line[position] = '\0';
    return appendDecodedQzssLine(messages, line);
}

bool appendDecodedQzssHex64(DecodedQzssMessages &messages, const char *name, uint64_t value)
{
    char line[64];
    static constexpr char hex[] = "0123456789ABCDEF";
    const size_t nameLength = std::strlen(name);
    if (nameLength + 20 >= sizeof(line))
        return false;
    std::memcpy(line, name, nameLength);
    size_t position = nameLength;
    line[position++] = '=';
    line[position++] = '0';
    line[position++] = 'x';
    for (int shift = 60; shift >= 0; shift -= 4)
        line[position++] = hex[(value >> shift) & 0xF];
    line[position] = '\0';
    return appendDecodedQzssLine(messages, line);
}

bool appendDecodedQzssBits(DecodedQzssMessages &messages, const uint8_t *frame, const char *name, uint16_t firstBit,
                            uint8_t bitCount)
{
    char line[meshtastic_Constants_DATA_PAYLOAD_LEN];
    const int prefix = std::snprintf(line, sizeof(line), "%s=", name);
    if (prefix < 0 || static_cast<size_t>(prefix) + bitCount >= sizeof(line))
        return false;
    for (uint8_t i = 0; i < bitCount; ++i)
        line[prefix + i] = getBit(frame, firstBit + i) ? '1' : '0';
    line[prefix + bitCount] = '\0';
    return appendDecodedQzssLine(messages, line);
}

bool findDcrReportEpoch(unsigned month, unsigned day, unsigned hour, unsigned minute, time_t &reportEpoch)
{
    const time_t now = static_cast<time_t>(getValidTime(RTCQualityFromNet));
    if (!now)
        return false;
    const struct tm *nowUtcValue = gmtime(&now);
    if (!nowUtcValue)
        return false;

    const struct tm nowUtc = *nowUtcValue;
    const int currentYear = nowUtc.tm_year;
    uint64_t closestDistance = UINT64_MAX;
    // MT43 omits the year, so use the date closest to the device's current time.
    for (int year = currentYear - 1; year <= currentYear + 1; ++year) {
        struct tm reportUtc = {};
        reportUtc.tm_year = year;
        reportUtc.tm_mon = static_cast<int>(month) - 1;
        reportUtc.tm_mday = static_cast<int>(day);
        reportUtc.tm_hour = static_cast<int>(hour);
        reportUtc.tm_min = static_cast<int>(minute);
        const time_t candidate = gm_mktime(&reportUtc);
        const struct tm *normalized = gmtime(&candidate);
        if (!normalized || normalized->tm_year != year || normalized->tm_mon != static_cast<int>(month) - 1 ||
            normalized->tm_mday != static_cast<int>(day))
            continue;

        const uint64_t distance =
            candidate > now ? static_cast<uint64_t>(candidate - now) : static_cast<uint64_t>(now - candidate);
        if (distance < closestDistance) {
            closestDistance = distance;
            reportEpoch = candidate;
        }
    }
    return reportEpoch != 0;
}

bool formatDcrLocalTime(time_t timestamp, char *output, size_t capacity,
                        const char *format = "%m/%d %H:%M")
{
    const struct tm *localTimeValue = localtime(&timestamp);
    return localTimeValue && strftime(output, capacity, format, localTimeValue) != 0;
}

bool formatDcxOnsetLocalTime(uint32_t onsetWeek, uint32_t onsetTimeOfWeek, char *output, size_t capacity)
{
    if (onsetWeek > 1 || onsetTimeOfWeek == 0 || onsetTimeOfWeek > 10080)
        return false;
    const time_t now = static_cast<time_t>(getValidTime(RTCQualityFromNet));
    if (!now)
        return false;
    const struct tm *nowUtcValue = gmtime(&now);
    if (!nowUtcValue)
        return false;
    struct tm weekStart = *nowUtcValue;
    weekStart.tm_mday -= (weekStart.tm_wday + 6) % 7;
    weekStart.tm_hour = 0;
    weekStart.tm_min = 0;
    weekStart.tm_sec = 0;
    const time_t currentWeekStart = gm_mktime(&weekStart);
    const time_t onsetEpoch = currentWeekStart + static_cast<time_t>(onsetWeek * 7 * SEC_PER_DAY +
                                                                     (onsetTimeOfWeek - 1) * SEC_PER_MIN);
    return formatDcrLocalTime(onsetEpoch, output, capacity);
}

void formatDcxOnsetDescription(uint32_t onsetWeek, uint32_t onsetTimeOfWeek, char *output, size_t capacity)
{
    static const char *const weekdays[] = {"月", "火", "水", "木", "金", "土", "日"};
    if (onsetTimeOfWeek == 0) {
        std::snprintf(output, capacity, "未使用");
    } else if (onsetTimeOfWeek <= 10080) {
        const uint32_t minuteOfWeek = onsetTimeOfWeek - 1;
        const uint32_t weekday = minuteOfWeek / 1440;
        const uint32_t minuteOfDay = minuteOfWeek % 1440;
        if (!formatDcxOnsetLocalTime(onsetWeek, onsetTimeOfWeek, output, capacity))
            std::snprintf(output, capacity, "%s%s曜 %02u:%02u UTC", onsetWeek ? "来週" : "今週", weekdays[weekday],
                          static_cast<unsigned>(minuteOfDay / 60), static_cast<unsigned>(minuteOfDay % 60));
    } else {
        std::snprintf(output, capacity, "未使用コード");
    }
}

time_t findDcrEventEpoch(time_t reportEpoch, unsigned day, unsigned hour, unsigned minute)
{
    const struct tm *reportUtcValue = gmtime(&reportEpoch);
    if (!reportUtcValue)
        return 0;
    const struct tm reportUtc = *reportUtcValue;
    time_t eventEpoch = 0;
    uint64_t closestDistance = UINT64_MAX;
    // The event fields omit month and year, so choose the matching date nearest the report time.
    for (int monthOffset = -1; monthOffset <= 1; ++monthOffset) {
        struct tm monthStart = reportUtc;
        monthStart.tm_mday = 1;
        monthStart.tm_mon += monthOffset;
        monthStart.tm_hour = 0;
        monthStart.tm_min = 0;
        monthStart.tm_sec = 0;
        const time_t monthEpoch = gm_mktime(&monthStart);
        const struct tm *monthUtcValue = gmtime(&monthEpoch);
        if (!monthUtcValue)
            continue;
        const struct tm monthUtc = *monthUtcValue;

        struct tm candidateTime = monthUtc;
        candidateTime.tm_mday = static_cast<int>(day);
        candidateTime.tm_hour = static_cast<int>(hour);
        candidateTime.tm_min = static_cast<int>(minute);
        candidateTime.tm_sec = 0;
        const time_t candidate = gm_mktime(&candidateTime);
        const struct tm *normalizedValue = gmtime(&candidate);
        if (!normalizedValue || normalizedValue->tm_year != monthUtc.tm_year ||
            normalizedValue->tm_mon != monthUtc.tm_mon || normalizedValue->tm_mday != static_cast<int>(day))
            continue;

        const uint64_t distance = candidate > reportEpoch ? static_cast<uint64_t>(candidate - reportEpoch)
                                                          : static_cast<uint64_t>(reportEpoch - candidate);
        if (distance < closestDistance) {
            closestDistance = distance;
            eventEpoch = candidate;
        }
    }
    return eventEpoch;
}

using DcrCode = QzssDcrCodeTables::Id;

constexpr size_t DCR_CHAT_CAPACITY =
#if HAS_SCREEN || defined(MESHTASTIC_INCLUDE_NICHE_GRAPHICS)
    MAX_MESSAGE_SIZE;
#else
    meshtastic_Constants_DATA_PAYLOAD_LEN;
#endif
static_assert(DCR_CHAT_CAPACITY <= meshtastic_Constants_DATA_PAYLOAD_LEN);
constexpr size_t DCR_CHAT_PART_NUMBER_RESERVE = sizeof(" (99/99)") - 1;
static_assert(MAX_DECODED_QZSS_MESSAGES <= 99);
class DcrChat
{
  public:
    DcrChat(DecodedQzssMessages &messages, std::string_view header) : messages(messages), header(header)
    {
        messages.count = 0;
        startPart();
    }

    size_t bodyCapacity() const { return DCR_CHAT_CAPACITY - header.size() - DCR_CHAT_PART_NUMBER_RESERVE - 2; }

    template <typename... Args> bool append(const char *format, Args... args)
    {
        char line[DCR_CHAT_CAPACITY];
        const int length = std::snprintf(line, sizeof(line), format, args...);
        if (length < 0 || static_cast<size_t>(length) >= sizeof(line))
            return false;
        return appendText(std::string_view(line, length));
    }

    bool appendText(std::string_view text)
    {
        constexpr std::string_view FULLWIDTH_SPACE = "　";
        const size_t bodyStart = header.size() + 1;
        const size_t capacity = bodyCapacity();
        while (!text.empty()) {
            const size_t index = messages.count - 1;
            const size_t used = messages.length[index];
            const bool hasBody = used > bodyStart;
            const size_t occupied = used - bodyStart + hasBody;
            const size_t available = occupied < capacity ? capacity - occupied : 0;
            size_t length = text.size();
            size_t advance = length;
            if (length > available) {
                if (hasBody && length <= capacity) {
                    if (!startPart())
                        return false;
                    continue;
                }
                const size_t space = text.find_last_of(" \t", available);
                const size_t fullwidthSpace = text.rfind(FULLWIDTH_SPACE, available);
                const bool useFullwidth = fullwidthSpace != text.npos && (space == text.npos || fullwidthSpace > space);
                const size_t split = useFullwidth ? fullwidthSpace : space;
                if (split != text.npos && split > 0) {
                    length = split;
                    advance = split + (useFullwidth ? FULLWIDTH_SPACE.size() : 1);
                } else if (hasBody) {
                    if (!startPart())
                        return false;
                    continue;
                } else {
                    length = available;
                    while (length && (static_cast<uint8_t>(text[length]) & 0xc0) == 0x80)
                        --length;
                    if (!length)
                        return false;
                    advance = length;
                }
            }

            char *tail = messages.text[index] + used;
            if (hasBody)
                *tail++ = ' ';
            std::memcpy(tail, text.data(), length);
            tail[length] = '\0';
            messages.length[index] += static_cast<uint16_t>(length + hasBody);
            text.remove_prefix(advance);
            if (!text.empty() && !startPart())
                return false;
        }
        return true;
    }

    bool finish()
    {
        if (messages.count < 2)
            return true;

        for (uint8_t i = 0; i < messages.count; ++i) {
            char part[DCR_CHAT_PART_NUMBER_RESERVE + 1];
            const int length = std::snprintf(part, sizeof(part), " (%u/%u)", static_cast<unsigned>(i + 1),
                                             static_cast<unsigned>(messages.count));
            if (length < 0 || static_cast<size_t>(length) >= sizeof(part))
                return false;

            char *tail = messages.text[i] + header.size();
            std::memmove(tail + length, tail, messages.length[i] - header.size() + 1);
            std::memcpy(tail, part, length);
            messages.length[i] += length;
        }
        return true;
    }

  private:
    bool startPart()
    {
        if (messages.count == MAX_DECODED_QZSS_MESSAGES)
            return false;
        const size_t index = messages.count++;
        char *text = messages.text[index];
        std::memcpy(text, header.data(), header.size());
        text[header.size()] = '\n';
        text[header.size() + 1] = '\0';
        messages.length[index] = static_cast<uint16_t>(header.size() + 1);
        return true;
    }

    DecodedQzssMessages &messages;
    std::string_view header;
};

const char *dcrName(DcrCode table, uint64_t code, const char *fallback)
{
    const char *name = QzssDcrCodeTables::lookup(table, code, nullptr, 0);
    return name ? name : fallback;
}

template <typename Select> bool appendQzssRegions(DcrChat &chat, const char *prefix, unsigned count, Select select)
{
    constexpr std::string_view separator = "、";
    char line[DCR_CHAT_CAPACITY];
    const size_t prefixLength = std::strlen(prefix);
    std::memcpy(line, prefix, prefixLength);
    size_t length = prefixLength;
    for (unsigned i = 0; i < count; ++i) {
        const char *region = select(i);
        if (!region)
            continue;
        const size_t regionLength = std::strlen(region);
        if (length > prefixLength && length + separator.size() + regionLength > chat.bodyCapacity()) {
            line[length] = '\0';
            if (!chat.append("%s", line))
                return false;
            length = prefixLength;
        }
        if (length > prefixLength) {
            std::memcpy(line + length, separator.data(), separator.size());
            length += separator.size();
        }
        if (length + regionLength > chat.bodyCapacity())
            return false;
        std::memcpy(line + length, region, regionLength);
        length += regionLength;
    }
    line[length] = '\0';
    return length == prefixLength || chat.append("%s", line);
}

template <typename Select> bool appendDcrRegions(DcrChat &chat, const char *prefix, DcrCode table, unsigned count, Select select)
{
    return appendQzssRegions(chat, prefix, count, [&](unsigned i) {
        const uint64_t code = select(i);
        return code ? dcrName(table, code, "地域名不明") : nullptr;
    });
}

const char *dcrComment(uint32_t code)
{
    switch (code) {
    case 104:
    case 213:
        return "海面変動が続く見込み。海水浴・磯釣り等に注意。";
    case 105:
    case 214:
        return "海面変動が続く見込み。磯釣り等に注意。";
    case 107:
        return "現在、津波警報・注意報の発表なし。";
    case 109:
        return "満潮と重なると津波が高くなる。厳重警戒。";
    case 110:
        return "満潮と重なると津波が高くなる。十分注意。";
    case 111:
        return "場所により観測値を超える津波が到達したおそれ。";
    case 113:
        return "沖合観測から、一部沿岸には津波が到達したと推定。";
    case 114:
        return "潮位変化から最大波の観測まで数時間以上かかることがあります。";
    case 121:
    case 122:
        return "沿岸・川沿いは直ちに高台などへ。解除まで戻らない。";
    case 132:
        return "場所により予想を超える高さの津波のおそれ。";
    case 142:
        return "沖合で高い津波を観測。大津波警報・津波警報に切替。";
    case 143:
        return "沖合で高い津波を観測。大津波警報・津波警報を変更。";
    case 144:
        return "沖合で高い津波を観測。大津波警報に切替。";
    case 145:
        return "沖合で高い津波を観測。大津波警報を変更。";
    case 146:
        return "沖合で高い津波を観測。津波警報に切替。";
    case 147:
        return "沖合で高い津波を観測。津波警報を変更。";
    case 148:
        return "沖合で高い津波を観測。予想される津波の高さを変更。";
    case 201:
        return "強い揺れに警戒。";
    case 211:
        return "津波警報等を発表中。";
    case 212:
        return "日本沿岸で若干の海面変動のおそれ。被害の心配なし。";
    case 215:
        return "この地震による津波の心配なし。";
    case 227:
        return "震源付近で小さな津波のおそれ。被害の心配なし。";
    case 228:
        return "この規模で震源が浅い海域の場合、津波のおそれ。";
    case 229:
        return "日本への津波は調査中。";
    case 242:
        return "緊急地震速報発表済み。観測最大震度2。";
    case 243:
        return "緊急地震速報発表済み。観測最大震度1。";
    case 244:
        return "緊急地震速報発表済み。震度1以上の観測なし。";
    case 245:
        return "緊急地震速報発表済み。強い揺れの観測なし。";
    default:
        return dcrName(DcrCode::NotificationOnDisasterPrevention, code, "防災コメント内容不明");
    }
}

bool appendDcrComments(DcrChat &chat, const uint8_t *frame, bool warningEnded)
{
    const char *comments[3] = {};
    const char *evacuation = nullptr;
    for (unsigned i = 0; i < 3; ++i) {
        const uint32_t code = getBits(frame, 53 + i * 9, 9);
        if (code == 121 || code == 122)
            evacuation = warningEnded ? nullptr : dcrComment(code);
        else if (code && !(warningEnded && code == 149))
            comments[i] = dcrComment(code);
    }
    if (evacuation && !chat.append("%s", evacuation))
        return false;
    for (unsigned i = 0; i < 3; ++i) {
        const char *comment = comments[i];
        if (!comment || (evacuation && std::strcmp(comment, "ただちに避難してください。") == 0) ||
            (i > 0 && comments[0] && std::strcmp(comment, comments[0]) == 0) ||
            (i > 1 && comments[1] && std::strcmp(comment, comments[1]) == 0))
            continue;
        if (!chat.append("%s", comment))
            return false;
    }
    return true;
}

time_t dcrEventTime(const uint8_t *frame, unsigned firstBit, time_t issued, char *out, size_t capacity)
{
    const unsigned day = getBits(frame, firstBit, 5);
    const unsigned hour = getBits(frame, firstBit + 5, 5);
    const unsigned minute = getBits(frame, firstBit + 10, 6);
    *out = '\0';
    if (!day || hour > 23 || minute > 59)
        return 0;
    const time_t event = issued ? findDcrEventEpoch(issued, day, hour, minute) : 0;
    if (!event || !formatDcrLocalTime(event, out, capacity))
        std::snprintf(out, capacity, "%u日 %02u:%02u UTC", day, hour, minute);
    return event;
}

bool appendDcrRange(DcrChat &chat, const char *label, const char *prefix, const char *lower, const char *upper)
{
    if (lower && upper && std::strcmp(lower, upper) != 0) {
        if (std::strncmp(upper, prefix, std::strlen(prefix)) == 0)
            return chat.append("%s%s%s〜%s", label, prefix, lower + std::strlen(prefix), upper + std::strlen(prefix));
        return chat.append("%s%s%s", label, lower, upper);
    }
    if (lower && !upper)
        return chat.append("%s%s（下限）", label, lower);
    if (!lower && upper)
        return chat.append("%s%s（上限）", label, upper);
    return !lower || chat.append("%s%s", label, lower);
}

bool formatDcrCoordinates(const uint8_t *frame, unsigned bit, char *out, size_t capacity)
{
    const unsigned latDegrees = getBits(frame, bit + 1, 7);
    const unsigned latMinutes = getBits(frame, bit + 8, 6);
    const unsigned latSeconds = getBits(frame, bit + 14, 6);
    const unsigned lonDegrees = getBits(frame, bit + 21, 8);
    const unsigned lonMinutes = getBits(frame, bit + 29, 6);
    const unsigned lonSeconds = getBits(frame, bit + 35, 6);
    if (latDegrees >= 90 || lonDegrees >= 180 || latMinutes >= 60 || lonMinutes >= 60 || latSeconds >= 60 || lonSeconds >= 60)
        return false;
    const unsigned lat = (latDegrees * 3600 + latMinutes * 60 + latSeconds + 18) / 36;
    const unsigned lon = (lonDegrees * 3600 + lonMinutes * 60 + lonSeconds + 18) / 36;
    std::snprintf(out, capacity, "%s%u.%02u° %s%u.%02u°", getBit(frame, bit) ? "南緯" : "北緯", lat / 100, lat % 100,
                  getBit(frame, bit + 20) ? "西経" : "東経", lon / 100, lon % 100);
    return true;
}

bool appendDcrEarthquake(DcrChat &chat, const uint8_t *frame, uint32_t hazard, time_t issued, bool cancelled)
{
    if (hazard == 1) {
        const uint32_t intensityLower = getBits(frame, 122, 4);
        const uint32_t intensityUpper = getBits(frame, 126, 4);
        const uint32_t motionLower = getBits(frame, 47, 3);
        const uint32_t motionUpper = getBits(frame, 50, 3);
        if (!cancelled) {
            const char *minIntensity =
                intensityLower <= 10 ? dcrName(DcrCode::SeismicIntensityLowerLimit, intensityLower, nullptr) : nullptr;
            const char *maxIntensity = intensityUpper <= (minIntensity ? 11u : 10u)
                                           ? dcrName(DcrCode::SeismicIntensityUpperLimit, intensityUpper, nullptr)
                                           : nullptr;
            const char *minMotion = motionLower && motionLower <= 5
                                        ? dcrName(DcrCode::LongPeriodGroundMotionLowerLimit, motionLower, nullptr)
                                        : nullptr;
            const char *maxMotion = motionUpper && motionUpper <= (minMotion ? 6u : 5u)
                                        ? dcrName(DcrCode::LongPeriodGroundMotionUpperLimit, motionUpper, nullptr)
                                        : nullptr;
            if (!appendDcrComments(chat, frame, false) || !appendDcrRange(chat, "予測", "震度", minIntensity, maxIntensity) ||
                !appendDcrRange(chat, "予測", "長周期地震動階級", minMotion, maxMotion))
                return false;
        }
        return appendDcrRegions(chat, "対象 ", DcrCode::EewForecastRegion, 80,
                                [&](unsigned i) { return getBit(frame, 130 + i) ? i + 1 : 0; });
    }

    char event[40];
    dcrEventTime(frame, hazard == 2 ? 80 : 53, issued, event, sizeof(event));
    if (*event && !chat.append("発生 %s", event))
        return false;
    if (cancelled)
        return true;

    if (hazard == 3) {
        for (unsigned intensity = 7; intensity; --intensity) {
            char label[32];
            std::snprintf(label, sizeof(label), "震度%s ", dcrName(DcrCode::SeismicIntensity, intensity, "不明"));
            if (!appendDcrRegions(chat, label, DcrCode::Prefecture, 16, [&](unsigned i) {
                    const unsigned bit = 69 + i * 9;
                    return getBits(frame, bit, 3) == intensity ? getBits(frame, bit + 3, 6) : 0;
                }))
                return false;
        }
        return true;
    }

    const uint32_t epicenterCode = getBits(frame, 112, 10);
    const char *epicenter = epicenterCode ? dcrName(DcrCode::EpicenterAndHypocenter, epicenterCode, nullptr) : nullptr;
    char coordinates[48];
    if (!epicenter && getBits64(frame, 122, 41) && formatDcrCoordinates(frame, 122, coordinates, sizeof(coordinates)))
        epicenter = coordinates;
    char magnitudeBuffer[24];
    char depthBuffer[24];
    const char *magnitude =
        QzssDcrCodeTables::lookup(DcrCode::HypocenterMagnitude, getBits(frame, 105, 7), magnitudeBuffer, sizeof(magnitudeBuffer));
    const char *depth =
        QzssDcrCodeTables::lookup(DcrCode::DepthOfHypocenter, getBits(frame, 96, 9), depthBuffer, sizeof(depthBuffer));
    return chat.append("%s%s%s%s%s", epicenter ? epicenter : "", magnitude ? " M" : "", magnitude ? magnitude : "",
                       depth ? " 深さ" : "", depth ? depth : "") &&
           appendDcrComments(chat, frame, false);
}

const char *dcrTsunamiWarning(uint32_t code)
{
    if (code == 2)
        return "津波警報解除";
    if (code == 5)
        return "大津波警報";
    return dcrName(DcrCode::TsunamiWarningCode, code, "津波情報（警報種別不明）");
}

uint32_t dcrSingleTsunamiRegion(const uint8_t *frame)
{
    const uint32_t region = getBits(frame, 100, 10);
    for (unsigned i = 1; i < 5; ++i) {
        if (getBits(frame, 100 + i * 26, 10))
            return 0;
    }
    return region;
}

bool appendDcrTsunami(DcrChat &chat, const uint8_t *frame, uint32_t hazard, time_t issued, bool cancelled, bool regionInTitle)
{
    const bool domestic = hazard == 5;
    const uint32_t warning = getBits(frame, 80, 4);
    const bool warningEnded = cancelled || (domestic && (warning == 1 || warning == 2));
    if (!cancelled && domestic && !appendDcrComments(chat, frame, warningEnded))
        return false;
    if (!domestic && !chat.append("%s", dcrName(DcrCode::TsunamigenicPotential, getBits(frame, 53, 3), "津波発生の可能性不明")))
        return false;

    const unsigned heightWidth = domestic ? 4 : 9;
    const unsigned regionWidth = domestic ? 10 : 7;
    for (unsigned i = 0; i < 5; ++i) {
        const unsigned bit = domestic ? 84 + i * 26 : 56 + i * 28;
        const uint32_t region = getBits(frame, bit + 12 + heightWidth, regionWidth);
        if (!region)
            continue;
        const char *name =
            regionInTitle ? ""
                          : dcrName(domestic ? DcrCode::TsunamiForecastRegion : DcrCode::CoastalRegion, region, "対象地域不明");
        if (warningEnded) {
            if (!chat.append("%s", name))
                return false;
            continue;
        }

        const unsigned offset = getBit(frame, bit);
        const unsigned hour = getBits(frame, bit + 1, 5);
        const unsigned minute = getBits(frame, bit + 6, 6);
        char arrival[48] = {};
        if (hour == 31 && minute == 63) {
            std::snprintf(arrival, sizeof(arrival), "%s", domestic ? "到達済み" : "到達済み、または時刻不明");
        } else if (hour <= 23 && minute <= 59) {
            char time[28];
            const time_t arrivalEpoch =
                issued + offset * SEC_PER_DAY + hour * SEC_PER_HOUR + minute * SEC_PER_MIN - (issued % SEC_PER_DAY);
            const struct tm *arrivalLocalValue = localtime(&arrivalEpoch);
            struct tm arrivalLocal = {};
            if (arrivalLocalValue)
                arrivalLocal = *arrivalLocalValue;
            const struct tm *issuedLocal = localtime(&issued);
            const bool sameDay = arrivalLocalValue && issuedLocal && arrivalLocal.tm_year == issuedLocal->tm_year &&
                                 arrivalLocal.tm_yday == issuedLocal->tm_yday;
            if (!issued || !formatDcrLocalTime(arrivalEpoch, time, sizeof(time), sameDay ? "%H:%M" : "%m/%d %H:%M"))
                std::snprintf(time, sizeof(time), "%s%02u:%02u UTC", offset ? "翌日 " : "当日 ", hour, minute);
            std::snprintf(arrival, sizeof(arrival), "%s到達予想", time);
        }

        const uint32_t heightCode = getBits(frame, bit + 12, heightWidth);
        const char *height =
            dcrName(domestic ? DcrCode::TsunamiHeight : DcrCode::NorthwestPacificTsunamiHeight, heightCode, "不明");
        if (domestic && heightCode == 13)
            height = nullptr;
        else if (!domestic && heightCode == 508)
            height = "10m超";
        if (!chat.append("%s%s%s%s%s", name, *name && *arrival ? " " : "", arrival,
                         height ? (*name || *arrival ? "／高さ" : "高さ") : "", height ? height : ""))
            return false;
    }
    return true;
}

const char *dcrNankaiStatus(uint32_t code)
{
    if (code >= 1 && code <= 3)
        return "調査中";
    return dcrName(DcrCode::InformationSerialCode, code, "区分不明");
}

bool appendDcrNankai(DcrChat &chat, const uint8_t *frame, bool training)
{
    constexpr unsigned PAGE_BYTES = 18;
    constexpr unsigned MAX_PAGES = 63;
    struct NankaiPages {
        uint64_t key = 0;
        uint64_t received = 0;
        uint32_t started = 0;
        char text[MAX_PAGES][PAGE_BYTES];
    };
    static NankaiPages pending[2];
    NankaiPages &pages = pending[training ? 1 : 0];
    const unsigned page = getBits(frame, 201, 6);
    const unsigned total = getBits(frame, 207, 6);
    if (!page || page > total)
        return true;

    const uint64_t key = (getBits64(frame, 14, 43) << 6) | total;
    const bool first = pages.key != key || Throttle::hasElapsed(pages.started, SEC_PER_DAY * 1000);
    if (first) {
        pages.key = key;
        pages.received = 0;
        pages.started = Time::getMillis();
    }
    const uint64_t pageMask = uint64_t{1} << (page - 1);
    if (pages.received & pageMask)
        return false;
    pages.received |= pageMask;
    for (unsigned i = 0; i < PAGE_BYTES; ++i)
        pages.text[page - 1][i] = static_cast<char>(getBits(frame, 57 + i * 8, 8));
    if (pages.received != (uint64_t{1} << total) - 1)
        return first;

    static char text[MAX_PAGES * PAGE_BYTES + 1];
    size_t size = 0;
    for (unsigned i = 0; i < total; ++i) {
        const size_t bytes = strnlen(pages.text[i], PAGE_BYTES);
        std::memcpy(text + size, pages.text[i], bytes);
        size += bytes;
    }
    text[size] = '\0';
    sanitizeUtf8(text, sizeof(text));
    for (char *c = text; *c; ++c) {
        if (*c == '\r' || *c == '\n')
            *c = ' ';
    }
    return chat.appendText(text);
}

bool appendDcrVolcano(DcrChat &chat, const uint8_t *frame, time_t issued, bool cancelled)
{
    const uint32_t ambiguity = getBits(frame, 50, 3);
    if (!cancelled && ambiguity <= 5) {
        char event[48] = {};
        if (ambiguity <= 3) {
            dcrEventTime(frame, 53, issued, event, sizeof(event));
        } else {
            const unsigned day = getBits(frame, 53, 5);
            const unsigned hour = getBits(frame, 58, 5);
            if (day && ambiguity == 5)
                std::snprintf(event, sizeof(event), "%u日 UTC", day);
            else if (day && hour <= 23) {
                const time_t time = issued ? findDcrEventEpoch(issued, day, hour, 0) : 0;
                char local[24];
                if (time && formatDcrLocalTime(time, local, sizeof(local), "%m/%d %H"))
                    std::snprintf(event, sizeof(event), "%s時", local);
                else
                    std::snprintf(event, sizeof(event), "%u日 %u時 UTC", day, hour);
            }
        }
        if (*event && !chat.append("活動 %s%s", event, ambiguity ? "ごろ" : ""))
            return false;
    }
    return appendDcrRegions(chat, "対象 ", DcrCode::LocalGovernment, 5,
                            [&](unsigned i) { return getBits(frame, 88 + i * 23, 23); });
}

bool appendDcrAshFall(DcrChat &chat, const uint8_t *frame, time_t issued, bool cancelled)
{
    char event[40];
    const time_t activity = dcrEventTime(frame, 53, issued, event, sizeof(event));
    for (unsigned i = 0; i < 4; ++i) {
        const unsigned bit = 83 + i * 29;
        const uint32_t region = getBits(frame, bit + 6, 23);
        if (!region)
            continue;
        const char *name = dcrName(DcrCode::LocalGovernment, region, "対象地域不明");
        if (cancelled) {
            if (!chat.append("%s", name))
                return false;
            continue;
        }
        const unsigned hours = getBits(frame, bit, 3);
        char time[40] = {};
        if (hours >= 1 && hours <= 6) {
            char local[24];
            if (activity) {
                const time_t forecast = activity + hours * SEC_PER_HOUR;
                if (formatDcrLocalTime(forecast, local, sizeof(local))) {
                    std::snprintf(time, sizeof(time), "%sごろ", local);
                } else {
                    const struct tm *utc = gmtime(&forecast);
                    if (utc && strftime(local, sizeof(local), "%m/%d %H:%M", utc))
                        std::snprintf(time, sizeof(time), "%s UTCごろ", local);
                }
            }
            if (!*time)
                std::snprintf(time, sizeof(time), "火山活動から%u時間後", hours);
        }
        if (!chat.append("%s %s%s%s", name, dcrName(DcrCode::AshFallWarningCode, getBits(frame, bit + 3, 3), "降灰内容不明"),
                         *time ? "／" : "", time))
            return false;
    }
    return true;
}

struct DcrRegionalWarnings {
    unsigned firstBit;
    unsigned warningBits;
    unsigned regionBits;
    unsigned count;
    DcrCode warningTable;
    DcrCode regionTable;

    unsigned recordBit(unsigned i) const { return firstBit + i * (warningBits + regionBits); }
    uint32_t warning(const uint8_t *frame, unsigned i) const { return getBits(frame, recordBit(i), warningBits); }
    uint64_t region(const uint8_t *frame, unsigned i) const { return getBits64(frame, recordBit(i) + warningBits, regionBits); }
};

constexpr DcrRegionalWarnings DCR_WEATHER{
    56, 5, 19, 6, DcrCode::WeatherRelatedDisasterSubCategory, DcrCode::WeatherForecastRegion};
constexpr DcrRegionalWarnings DCR_FLOOD{53, 4, 40, 3, DcrCode::FloodWarningLevel, DcrCode::FloodForecastRegion};
constexpr DcrRegionalWarnings DCR_MARINE{53, 5, 14, 8, DcrCode::MarineWarningCode, DcrCode::MarineForecastRegion};

const char *dcrCommonWarning(const uint8_t *frame, const DcrRegionalWarnings &layout)
{
    uint32_t code = UINT32_MAX;
    for (unsigned i = 0; i < layout.count; ++i) {
        if (!layout.region(frame, i))
            continue;
        const uint32_t warning = layout.warning(frame, i);
        if (code != UINT32_MAX && code != warning)
            return nullptr;
        code = warning;
    }
    return dcrName(layout.warningTable, code, nullptr);
}

bool appendDcrRegionalWarnings(DcrChat &chat, const uint8_t *frame, const DcrRegionalWarnings &layout, const char *state,
                               bool warningInTitle)
{
    if (warningInTitle)
        return appendDcrRegions(chat, "", layout.regionTable, layout.count, [&](unsigned i) { return layout.region(frame, i); });

    uint32_t seen = 0;
    for (unsigned i = 0; i < layout.count; ++i) {
        const uint32_t code = layout.warning(frame, i);
        const uint32_t mask = uint32_t{1} << code;
        if (!layout.region(frame, i) || (seen & mask))
            continue;
        seen |= mask;
        char label[96];
        std::snprintf(label, sizeof(label), "%s%s ", dcrName(layout.warningTable, code, "警報種別不明"), state);
        if (!appendDcrRegions(chat, label, layout.regionTable, layout.count,
                              [&](unsigned j) { return layout.warning(frame, j) == code ? layout.region(frame, j) : 0; }))
            return false;
    }
    return true;
}

bool appendDcrTyphoon(DcrChat &chat, const uint8_t *frame, time_t issued)
{
    char event[40];
    dcrEventTime(frame, 53, issued, event, sizeof(event));
    const char *type = dcrName(DcrCode::TyphoonReferenceTimeType, getBits(frame, 69, 3), "基点");
    if (*event && !chat.append("%s %s", type, event))
        return false;

    const uint32_t scaleCode = getBits(frame, 94, 4);
    const uint32_t intensityCode = getBits(frame, 98, 4);
    const char *scale = scaleCode ? dcrName(DcrCode::TyphoonScaleCategory, scaleCode, "") : "";
    const char *intensity = intensityCode ? dcrName(DcrCode::TyphoonIntensityCategory, intensityCode, "") : "";
    const unsigned wind = getBits(frame, 154, 7);
    char windText[32] = {};
    if (wind >= 15 && wind <= 105)
        std::snprintf(windText, sizeof(windText), "最大風速%u m/s", wind);
    if (!chat.append("%s%s%s%s%s", scale, *scale && *intensity ? "・" : "", intensity,
                     (*scale || *intensity) && *windText ? "／" : "", windText))
        return false;
    char coordinates[48];
    return !formatDcrCoordinates(frame, 102, coordinates, sizeof(coordinates)) || chat.append("中心 %s", coordinates);
}

bool formatDcrMessage(const uint8_t *frame, DecodedQzssMessages &messages)
{
    const uint32_t hazard = getBits(frame, 17, 4);
    const uint32_t info = getBits(frame, 41, 2);
    const bool training = getBits(frame, 14, 3) == 7;
    const char *kind = dcrInformationTypeText(hazard, info);
    const bool cancelled = std::strcmp(kind, "取消") == 0;
    const char *category = dcrHazardText(hazard);
    const char *name = category;
    const DcrRegionalWarnings *regional = nullptr;
    bool regionInTitle = false;
    char subject[96];
    switch (hazard) {
    case 4:
        std::snprintf(subject, sizeof(subject), "南海トラフ地震臨時情報（%s）", dcrNankaiStatus(getBits(frame, 53, 4)));
        name = subject;
        break;
    case 5: {
        if (!cancelled)
            name = dcrTsunamiWarning(getBits(frame, 80, 4));
        const char *region = dcrName(DcrCode::TsunamiForecastRegion, dcrSingleTsunamiRegion(frame), nullptr);
        if (region) {
            std::snprintf(subject, sizeof(subject), "%s %s", name, region);
            name = subject;
            regionInTitle = true;
        }
        break;
    }
    case 8: {
        const unsigned warningCode = getBits(frame, 69, 7);
        std::snprintf(subject, sizeof(subject), "%s %s%s", dcrName(DcrCode::VolcanoName, getBits(frame, 76, 12), "火山情報"),
                      warningCode >= 11 && warningCode <= 15 ? "噴火警戒" : "",
                      dcrName(DcrCode::VolcanicWarningCode, warningCode, "警報種別不明"));
        name = subject;
        break;
    }
    case 9:
        std::snprintf(subject, sizeof(subject), "%s 降灰予報（%s）",
                      dcrName(DcrCode::VolcanoName, getBits(frame, 71, 12), "火山名不明"),
                      dcrName(DcrCode::AshFallWarningType, getBits(frame, 69, 2), "種別不明"));
        name = subject;
        break;
    case 10:
        regional = &DCR_WEATHER;
        break;
    case 11:
        regional = &DCR_FLOOD;
        break;
    case 12: {
        const unsigned number = getBits(frame, 87, 7);
        if (number >= 1 && number <= 99) {
            std::snprintf(subject, sizeof(subject), "台風%u号", number);
            name = subject;
        }
        break;
    }
    case 14:
        regional = &DCR_MARINE;
        break;
    default:
        break;
    }

    const bool weather = !cancelled && hazard == 10;
    const unsigned weatherState = getBits(frame, 53, 3);
    const bool weatherCleared = weather && weatherState == 2;
    const char *warningState = weather && weatherState != 1 && !weatherCleared ? " 状態不明" : "";
    const char *warning = regional ? dcrCommonWarning(frame, *regional) : nullptr;
    if (warning) {
        std::snprintf(subject, sizeof(subject), "%s%s", warning, warningState);
        name = subject;
    }

    const unsigned month = getBits(frame, 21, 4);
    const unsigned day = getBits(frame, 25, 5);
    const unsigned hour = getBits(frame, 30, 5);
    const unsigned minute = getBits(frame, 35, 6);
    time_t issued = 0;
    char issuedText[40] = "発表時刻不明";
    if (month >= 1 && month <= 12 && day >= 1 && day <= 31 && hour <= 23 && minute <= 59) {
        if (!findDcrReportEpoch(month, day, hour, minute, issued) ||
            !formatDcrLocalTime(issued, issuedText, sizeof(issuedText), "%m/%d %H:%M 発表"))
            std::snprintf(issuedText, sizeof(issuedText), "%02u/%02u %02u:%02u UTC 発表", month, day, hour, minute);
    }

    const char *status = cancelled ? " 【取消】" : std::strcmp(kind, "訂正") == 0 ? " 【訂正】" : "";
    char header[96];
    std::snprintf(header, sizeof(header), "みちびきDCR%s%s%s %s", training ? " 【訓練・試験】" : "", status,
                  weatherCleared ? " 【解除】" : "", issuedText);
    DcrChat chat(messages, header);
    if (!chat.append("[%s]", category) || (std::strcmp(name, category) != 0 && !chat.append("%s", name)))
        return false;
    bool ok = true;
    switch (hazard) {
    case 1:
    case 2:
    case 3:
        ok = appendDcrEarthquake(chat, frame, hazard, issued, cancelled);
        break;
    case 4:
        ok = cancelled || appendDcrNankai(chat, frame, training);
        break;
    case 5:
    case 6:
        ok = appendDcrTsunami(chat, frame, hazard, issued, cancelled, regionInTitle);
        break;
    case 8:
        ok = appendDcrVolcano(chat, frame, issued, cancelled);
        break;
    case 9:
        ok = appendDcrAshFall(chat, frame, issued, cancelled);
        break;
    case 10:
    case 11:
    case 14:
        ok = appendDcrRegionalWarnings(chat, frame, *regional, warningState, warning != nullptr);
        break;
    case 12:
        ok = cancelled || appendDcrTyphoon(chat, frame, issued);
        break;
    }
    return ok && chat.finish();
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
    std::snprintf(text, capacity, "行動指示の詳細は不明");
    return true;
}

uint32_t dcxEllipseRadiusMilliKm(uint32_t code)
{
    static constexpr uint32_t radiusMilliKm[] = {216,      292,      395,      535,      723,      978,      1322,     1788,
                                                  2418,     3269,     4421,     5979,     8085,     10933,    14784,    19992,
                                                  27035,    36559,    49439,    66855,    90407,    122255,   165324,   223564,
                                                  302322,   408824,   552846,   747603,   1010970,  1367116,  1848727, 2500000};
    return code < sizeof(radiusMilliKm) / sizeof(radiusMilliKm[0]) ? radiusMilliKm[code] : 0;
}

bool appendDcxEllipseDetails(DecodedQzssMessages &messages, const uint8_t *frame)
{
    const uint32_t a12 = getBits(frame, 80, 16);
    const uint32_t a13 = getBits(frame, 96, 17);
    const uint32_t a14 = getBits(frame, 113, 5);
    const uint32_t a15 = getBits(frame, 118, 5);
    const uint32_t a16 = getBits(frame, 123, 6);
    const int32_t latitudeMilliDegrees = -90000 + static_cast<int32_t>((static_cast<uint64_t>(a12) * 180000 + 32767) / 65535);
    const int32_t longitudeMilliDegrees = -180000 + static_cast<int32_t>((static_cast<uint64_t>(a13) * 360000 + 65535) / 131071);
    const uint32_t majorRadius = dcxEllipseRadiusMilliKm(a14);
    const uint32_t minorRadius = dcxEllipseRadiusMilliKm(a15);
    const int32_t azimuthCentiDegrees = -9000 + static_cast<int32_t>((a16 * 18000 + 32) / 64);
    return appendDecodedQzssFixed(messages, "A12/楕円中心緯度", latitudeMilliDegrees, 3, "°") &&
           appendDecodedQzssFixed(messages, "A13/楕円中心経度", longitudeMilliDegrees, 3, "°") &&
           appendDecodedQzssFixed(messages, "A14/長半径", static_cast<int32_t>(majorRadius), 3, "km") &&
           appendDecodedQzssFixed(messages, "A15/短半径", static_cast<int32_t>(minorRadius), 3, "km") &&
           appendDecodedQzssFixed(messages, "A16/楕円方位", azimuthCentiDegrees, 2, "°");
}

bool appendDcxA18Fields(DecodedQzssMessages &messages, const uint8_t *frame, uint32_t hazard, DcrChat *chat = nullptr)
{
    const uint32_t settingType = getBits(frame, 129, 2);
    char label[64];
    if (settingType == 0) {
        static constexpr uint16_t centerRefinementMicroDegrees[] = {0, 343, 687, 1030, 1373, 1717, 2060, 2403};
        for (uint8_t i = 0; i < 2; ++i) {
            const uint32_t value = getBits(frame, 131 + i * 3, 3);
            std::snprintf(label, sizeof(label), "C%u/%s補正(コード=%u)", static_cast<unsigned>(i + 1), i == 0 ? "緯度" : "経度",
                          static_cast<unsigned>(value));
            if (!appendDecodedQzssFixed(messages, label, centerRefinementMicroDegrees[value], 6, "°"))
                return false;
        }
        for (uint8_t i = 0; i < 2; ++i) {
            const uint32_t value = getBits(frame, 137 + i * 3, 3);
            std::snprintf(label, sizeof(label), "C%u/%s補間率(コード=%u)", static_cast<unsigned>(i + 3),
                          i == 0 ? "長半径" : "短半径", static_cast<unsigned>(value));
            if (!appendDecodedQzssFixed(messages, label, static_cast<int32_t>(value * 125), 3, ""))
                return false;
        }
        return appendDecodedQzssNumber(messages, "A18/B1予約", getBits(frame, 143, 3));
    }

    if (settingType == 1) {
        const uint32_t latitude = getBits(frame, 131, 7);
        const uint32_t longitude = getBits(frame, 138, 7);
        const int32_t latitudeDelta = -1000000 + static_cast<int32_t>(latitude * 15625);
        const int32_t longitudeDelta = -1000000 + static_cast<int32_t>(longitude * 15625);
        std::snprintf(label, sizeof(label), "C5/中心緯度差(コード=%u)", static_cast<unsigned>(latitude));
        if (!appendDecodedQzssFixed(messages, label, latitudeDelta, 5, "°"))
            return false;
        std::snprintf(label, sizeof(label), "C6/中心経度差(コード=%u)", static_cast<unsigned>(longitude));
        return appendDecodedQzssFixed(messages, label, longitudeDelta, 5, "°") &&
               appendDecodedQzssNumber(messages, "A18/B2予約", getBits(frame, 145, 1));
    }

    if (settingType == 2) {
        static constexpr char homotheticFactors[][6] = {"0.25", "0.50", "0.75", "1.00", "1.25", "1.50", "1.75", "2.00"};
        const uint32_t shift = getBits(frame, 131, 2);
        const uint32_t factor = getBits(frame, 133, 3);
        const uint32_t bearing = getBits(frame, 136, 5);
        const uint32_t instruction = getBits(frame, 141, 5);
        char shiftText[16];
        char factorText[24];
        char instructionText[96];
        std::snprintf(label, sizeof(label), "C9/第2楕円方位(コード=%u)", static_cast<unsigned>(bearing));
        std::snprintf(shiftText, sizeof(shiftText), "%u/4長半径", static_cast<unsigned>(shift));
        std::snprintf(factorText, sizeof(factorText), "%s倍", homotheticFactors[factor]);
        std::snprintf(instructionText, sizeof(instructionText), "%s", dcxSecondEllipseInstruction(instruction));
        return appendDecodedQzssValue(messages, "C7/第2楕円中心シフト", shift, shiftText) &&
               appendDecodedQzssValue(messages, "C8/第2楕円軸倍率", factor, factorText) &&
               appendDecodedQzssFixed(messages, label, static_cast<int32_t>(bearing * 1125), 2, "°") &&
               appendDecodedQzssValue(messages, "C10/第2楕円行動指示", instruction + 1, instructionText);
    }

    struct DcxB4Field {
        const char *name;
        uint8_t width;
    };
    struct DcxB4Schema {
        uint8_t hazard;
        const DcxB4Field *fields;
        uint8_t count;
    };
    static constexpr DcxB4Field earthquake[] = {
        {"D1/マグニチュード", 4}, {"D2/震度係数", 3}, {"D3/震央方位", 4}, {"D4/震央距離", 4}};
    static constexpr DcxB4Field tidalWave[] = {{"D5/波高", 3}};
    static constexpr DcxB4Field temperature[] = {{"D6/気温範囲", 4}};
    static constexpr DcxB4Field hurricane[] = {{"D7/ハリケーン分類", 3}, {"D8/風速", 4}, {"D9/降雨量", 3}};
    static constexpr DcxB4Field typhoon[] = {{"D36/台風分類", 3}, {"D8/風速", 4}, {"D9/降雨量", 3}};
    static constexpr DcxB4Field tornado[] = {{"D8/風速", 4}, {"D9/降雨量", 3}, {"D11/竜巻確率", 3}};
    static constexpr DcxB4Field storm[] = {{"D8/風速", 4}, {"D9/降雨量", 3}, {"D10/被害分類", 3}, {"D16/雷強度", 3}};
    static constexpr DcxB4Field hail[] = {{"D12/雹スケール", 4}};
    static constexpr DcxB4Field rainfall[] = {{"D9/降雨量", 3}, {"D13/視程", 4}};
    static constexpr DcxB4Field snowfall[] = {{"D14/積雪深", 5}, {"D13/視程", 4}};
    static constexpr DcxB4Field flood[] = {{"D15/洪水深刻度", 2}};
    static constexpr DcxB4Field lightning[] = {{"D16/雷強度", 3}};
    static constexpr DcxB4Field windChill[] = {{"D8/風速", 4}, {"D6/気温範囲", 4}};
    static constexpr DcxB4Field derecho[] = {{"D8/風速", 4}, {"D9/降雨量", 3}, {"D16/雷強度", 3}, {"D11/竜巻確率", 3}};
    static constexpr DcxB4Field fog[] = {{"D17/霧レベル", 3}, {"D13/視程", 4}};
    static constexpr DcxB4Field blizzard[] = {{"D13/視程", 4}, {"D8/風速", 4}};
    static constexpr DcxB4Field drought[] = {{"D18/干ばつレベル", 2}};
    static constexpr DcxB4Field avalanche[] = {{"D19/雪崩警戒レベル", 3}};
    static constexpr DcxB4Field ashfall[] = {{"D20/降灰量・影響", 3}};
    static constexpr DcxB4Field windWave[] = {{"D8/風速", 4}, {"D5/波高", 3}};
    static constexpr DcxB4Field geomagnetic[] = {{"D21/地磁気スケール", 3}};
    static constexpr DcxB4Field terrorism[] = {{"D22/テロ脅威レベル", 3}};
    static constexpr DcxB4Field fireRisk[] = {{"D23/火災危険度", 3}};
    static constexpr DcxB4Field waterQuality[] = {{"D24/水質", 3}};
    static constexpr DcxB4Field uv[] = {{"D25/紫外線指数", 4}};
    static constexpr DcxB4Field infection[] = {{"D26/感染者数(10万人あたり)", 5}, {"D35/感染症種別", 6}};
    static constexpr DcxB4Field noise[] = {{"D27/騒音範囲", 4}};
    static constexpr DcxB4Field airQuality[] = {{"D28/大気汚染指数", 3}};
    static constexpr DcxB4Field outage[] = {{"D29/停止見込み時間", 5}};
    static constexpr DcxB4Field nuclear[] = {{"D30/原子力事象スケール", 4}};
    static constexpr DcxB4Field chemical[] = {{"D31/化学物質分類", 4}};
    static constexpr DcxB4Field biological[] = {{"D32/生物災害レベル", 2}, {"D33/生物災害種別", 2}};
    static constexpr DcxB4Field explosive[] = {{"D34/爆発物危険種別", 2}};
    static const DcxB4Schema schemas[] = {
        {36, earthquake, 4},   {43, tidalWave, 1},    {44, tidalWave, 1},    {63, temperature, 1}, {71, temperature, 1},
        {80, hurricane, 3},    {82, typhoon, 3},      {79, tornado, 3},      {77, storm, 4},       {70, hail, 1},
        {74, rainfall, 2},     {76, snowfall, 2},     {68, flood, 1},        {72, lightning, 1},   {81, windChill, 2},
        {64, derecho, 4},      {69, fog, 2},          {75, blizzard, 2},     {65, drought, 1},     {33, avalanche, 1},
        {32, ashfall, 1},      {47, windWave, 2},     {37, geomagnetic, 1},  {30, fireRisk, 1},    {27, fireRisk, 1},
        {16, waterQuality, 1}, {18, waterQuality, 1}, {21, waterQuality, 1}, {23, uv, 1},          {53, infection, 2},
        {51, infection, 2},    {19, noise, 1},        {15, airQuality, 1},   {56, outage, 1},      {57, outage, 1},
        {58, outage, 1},       {55, outage, 1},       {60, outage, 1},       {9, nuclear, 1},      {10, nuclear, 1},
        {11, nuclear, 1},      {5, chemical, 1},      {4, biological, 2},    {6, explosive, 1},    {103, terrorism, 1}};
    const DcxB4Schema *schema = nullptr;
    for (const DcxB4Schema &candidate : schemas) {
        if (candidate.hazard == hazard) {
            schema = &candidate;
            break;
        }
    }
    if (!schema)
        return chat || appendDecodedQzssBits(messages, frame, "A18/B4未対応詳細15ビット", 131, 15);

    uint16_t firstBit = 131;
    for (uint8_t i = 0; i < schema->count; ++i) {
        const DcxB4Field &field = schema->fields[i];
        const uint8_t fieldId = static_cast<uint8_t>((field.name[1] - '0') * (field.name[2] == '/' ? 1 : 10) +
                                                     (field.name[2] == '/' ? 0 : field.name[2] - '0'));
        const uint32_t value = getBits(frame, firstBit, field.width);
        const char *meaning = dcxHazardDetailText(fieldId, value);
        if (chat) {
            const char *name = std::strchr(field.name, '/') + 1;
            if (meaning && std::strcmp(meaning, "未定義コード") != 0 && !chat->append("%s %s", name, meaning))
                return false;
        } else if (!appendDecodedQzssValue(messages, field.name, value, meaning)) {
            return false;
        }
        firstBit += field.width;
    }
    const uint8_t reservedBitCount = static_cast<uint8_t>(146 - firstBit);
    return chat || reservedBitCount == 0 ||
           appendDecodedQzssBits(messages, frame, "A18/B4予約ビット", firstBit, reservedBitCount);
}

struct DcxJAlertInstruction {
    const char *headline;
    const char *action;
    bool allClear = false;
};

const DcxJAlertInstruction *dcxJAlertInstruction(uint32_t code)
{
    static constexpr DcxJAlertInstruction instructions[] = {
        {"ミサイル発射の可能性", "建物内か地下へ避難。"},
        {"ミサイルは通過したとみられる", "不審物に近づかず、直ちに警察・消防へ通報。", true},
        {"ミサイルは海に落下したとみられる", "不審物に近づかず、直ちに警察・消防へ通報。", true},
        {"ミサイルは日本に飛来しない見込み", "", true},
        {"ミサイルが周辺に落下するおそれ", "直ちに建物内か地下へ避難。"},
        {"ミサイル迎撃・破片落下のおそれ", "屋内避難を続け、続報を確認。"},
        {"ミサイルは周辺に落下したとみられる", "屋内避難を続け、続報を確認。"},
        {"ミサイルは日本に落下しない見込み", "", true},
    };
    return code >= 128 && code <= 135 ? &instructions[code - 128] : nullptr;
}

bool appendDcxChatInstruction(DcrChat &chat, bool japan, bool regionalLibrary, uint32_t code, const DcxJAlertInstruction *jAlert)
{
    if (!code)
        return true;
    if (jAlert)
        return chat.append("%s", jAlert->action);
    if (japan) {
        if (code == 126 || code == 136)
            return true;
        char text[144];
        formatDcxInstruction(code, text, sizeof(text));
        return chat.append("%s", code < 128 || code > 135 ? text : "行動指示の詳細は不明");
    }
    if (regionalLibrary)
        return chat.append("%s", "行動指示の詳細は不明");

    const uint32_t actionA = code >> 5;
    const uint32_t actionB = code & 0x1f;
    const char *a = dcxInternationalInstructionA(actionA);
    const char *b = dcxInternationalInstructionB(actionB);
    return (!actionA || chat.append("%s", a)) && (!actionB || std::strcmp(a, b) == 0 || chat.append("%s", b));
}

bool appendDcxChatEllipse(DcrChat &chat, const uint8_t *frame, const char *label, bool additional)
{
    const uint32_t lat = getBits(frame, additional ? 163 : 80, additional ? 17 : 16);
    const uint32_t lon = getBits(frame, additional ? 180 : 96, 17);
    const uint32_t major = getBits(frame, additional ? 197 : 113, 5);
    const uint32_t minor = getBits(frame, additional ? 202 : 118, 5);
    const uint32_t angle = getBits(frame, additional ? 207 : 123, additional ? 7 : 6);
    const double latitude = -90.0 + lat * 180.0 / (additional ? 131071 : 65535);
    double longitude = additional ? 45.0 + lon * 180.0 / 131071 : -180.0 + lon * 360.0 / 131071;
    if (longitude > 180.0)
        longitude -= 360.0;
    const double tilt = -90.0 + angle * 180.0 / (additional ? 128 : 64);
    return chat.append("%s %.3f°, %.3f°／半径%g×%gkm／東基準%+.0f°", label, latitude, longitude,
                       dcxEllipseRadiusMilliKm(major) / 1000.0, dcxEllipseRadiusMilliKm(minor) / 1000.0, tilt);
}

const char *dcxMunicipality(uint32_t code)
{
    if (!code)
        return nullptr;
    const char *name = QzssMunicipalityNames::find(static_cast<uint16_t>(code));
    return name ? name : "市区町村名不明";
}

bool appendDcxChatTarget(DcrChat &chat, const uint8_t *frame, bool japan, bool jAlert)
{
    if (jAlert) {
        if (getBit(frame, 146)) {
            if (!getBits64(frame, 147, 64))
                return chat.append("%s", "対象地域の指定なし");
            return appendQzssRegions(chat, "対象 ", 4,
                                     [&](unsigned i) { return dcxMunicipality(getBits(frame, 147 + i * 16, 16)); });
        }
        const uint64_t prefectures = getBits64(frame, 147, 47);
        if (!prefectures)
            return chat.append("%s", "対象地域の指定なし");
        if (prefectures == ((uint64_t{1} << 47) - 1))
            return chat.append("%s", "対象 全国");
        return appendDcrRegions(chat, "対象 ", DcrCode::Prefecture, 47,
                                [&](unsigned i) { return (prefectures & (uint64_t{1} << i)) ? i + 1 : 0; });
    }
    if (getBits64(frame, 80, 49))
        return appendDcxChatEllipse(chat, frame, "対象範囲の目安", false);
    if (!japan)
        return true;
    const char *municipality = dcxMunicipality(getBits(frame, 146, 16));
    return !municipality || chat.append("対象 %s", municipality);
}

bool formatDcxChatMessage(const uint8_t *frame, DecodedQzssMessages &messages)
{
    const uint32_t type = getBits(frame, 24, 2);
    const uint32_t country = getBits(frame, 26, 9);
    const uint32_t provider = getBits(frame, 35, 5);
    const uint32_t hazard = getBits(frame, 40, 7);
    const uint32_t severity = getBits(frame, 47, 2);
    const bool japan = country == 111;
    const bool jAlert = japan && (provider == 2 || provider == 3);
    const bool local = japan && provider == 4;
    if (japan && (provider < 1 || provider > 4))
        return false;

    const uint32_t code = getBits(frame, 70, 10);
    const bool regionalLibrary = getBit(frame, 66);
    const bool supportedLibrary = !japan || (regionalLibrary && getBits(frame, 67, 3) == 0);
    const bool instructionTest = japan && supportedLibrary && (code == 126 || (jAlert && code == 136));
    const bool test = type == 0 || hazard == 13 || hazard == 113 || instructionTest;
    const DcxJAlertInstruction *instruction = jAlert && supportedLibrary ? dcxJAlertInstruction(code) : nullptr;
    const bool cleared = type == 3 || (instruction && instruction->allClear);
    const char *status = test        ? "【試験】"
                         : type == 3 ? (jAlert ? "【誤報取消】" : "【解除】")
                         : cleared   ? "【避難解除】"
                         : type == 2 ? "【更新】"
                                     : "【警報】";
    const char *category = hazard > 0 && hazard <= 113 ? dcxHazardText(hazard) : test ? "災害情報" : "災害種別不明";
    const char *headline = instruction && type != 3 && !instructionTest ? instruction->headline : "";
    const char *source = jAlert ? " Jアラート" : local ? " 自治体" : japan ? " Lアラート" : "";
    const bool showSeverity = severity && !test && !cleared && !jAlert;

    char header[40];
    std::snprintf(header, sizeof(header), "みちびきDCX %s", status);
    DcrChat chat(messages, header);
    if (!chat.append("[%s]%s%s%s%s%s", category, source, *headline ? " " : "", headline, showSeverity ? " " : "",
                     showSeverity ? dcxSeverityText(severity) : ""))
        return false;
    if (!instructionTest && type != 3 && code) {
        if (!supportedLibrary) {
            if (!chat.append("%s", "行動指示の詳細は不明"))
                return false;
        } else if (!appendDcxChatInstruction(chat, japan, regionalLibrary, code, instruction)) {
            return false;
        }
    }
    if (local && !cleared && getBits64(frame, 162, 52) &&
        !appendDcxChatEllipse(chat, frame, getBit(frame, 162) ? "この範囲へ避難" : "この範囲から離れる", true))
        return false;
    if (!appendDcxChatTarget(chat, frame, japan, jAlert))
        return false;

    if (!cleared) {
        const uint32_t onset = getBits(frame, 50, 14);
        if (onset > 0 && onset <= 10080) {
            char time[64];
            formatDcxOnsetDescription(getBit(frame, 49), onset, time, sizeof(time));
            if (!chat.append("発生見込み %s", time))
                return false;
        }
        static const char *const durations[] = {"", "6時間未満", "6時間以上12時間未満", "12時間以上24時間未満"};
        const uint32_t duration = getBits(frame, 64, 2);
        if (duration && !chat.append("有効期間 %s", durations[duration]))
            return false;
        if (!japan && getBits(frame, 129, 2) == 3 && !appendDcxA18Fields(messages, frame, hazard, &chat))
            return false;
    }
    if (!japan) {
        const char *countryName = dcxCountryText(country);
        if (std::strcmp(countryName, "予約") == 0)
            return chat.finish();
        const char *providerName = dcxProviderText(country, provider);
        const bool namedProvider =
            provider && std::strcmp(providerName, "予約") != 0 && std::strcmp(providerName, "国・地域別定義") != 0;
        if (!chat.append("配信元 %s%s%s", countryName, namedProvider ? "・" : "", namedProvider ? providerName : ""))
            return false;
    }
    return chat.finish();
}

bool formatDcxAllFields(const uint8_t *frame, uint8_t satelliteId, DecodedQzssMessages &messages)
{
    messages.count = 1;
    messages.length[0] = 0;
    messages.text[0][0] = '\0';
    if (!appendDecodedQzssLine(messages, "MT44詳細"))
        return false;

    const uint32_t messageType = getBits(frame, 24, 2);
    const uint32_t country = getBits(frame, 26, 9);
    const uint32_t provider = getBits(frame, 35, 5);
    const uint32_t hazard = getBits(frame, 40, 7);
    const uint32_t severity = getBits(frame, 47, 2);
    const uint32_t onsetWeek = getBits(frame, 49, 1);
    const uint32_t onsetTimeOfWeek = getBits(frame, 50, 14);
    const uint32_t duration = getBits(frame, 64, 2);
    const uint32_t library = getBits(frame, 66, 1);
    const uint32_t libraryVersion = getBits(frame, 67, 3);
    const uint32_t instruction = getBits(frame, 70, 10);
    const uint32_t satelliteMaskType = getBits(frame, 14, 1);
    static const char *const messageTypes[] = {"試験", "警報", "更新", "解除"};
    char line[200];

    static const char *const severities[] = {"不明", "中程度", "重大", "極めて重大"};
    static const char *const durations[] = {"不明", "6時間未満", "6-12時間", "12-24時間"};
    static const char *const specificSettings[] = {"B1 主楕円の高精度化", "B2 災害中心位置", "B3 第2楕円", "B4 災害詳細"};
    static const char *const libraries[] = {"国際指示表", "国・地域指示表"};
    const char *countryText = dcxCountryText(country);
    const char *providerText = dcxProviderText(country, provider);
    const bool japanJAlert = country == 111 && (provider == 2 || provider == 3);
    char a7Description[64];
    char libraryVersionText[12];
    std::snprintf(libraryVersionText, sizeof(libraryVersionText), "第%u版", static_cast<unsigned>(libraryVersion + 1));
    formatDcxOnsetDescription(onsetWeek, onsetTimeOfWeek, a7Description, sizeof(a7Description));

    if (!appendDecodedQzssHex(messages, "PAB", getBits(frame, 0, 8), 2) ||
        !appendDecodedQzssNumber(messages, "MT", getBits(frame, 8, 6)) ||
        !appendDecodedQzssNumber(messages, "SD/衛星指定10ビット", getBits(frame, 14, 10)) ||
        !appendDecodedQzssValue(messages, "SDMT/SDマスク種別", satelliteMaskType,
                                satelliteMaskType ? "MT44送信状態" : "サービス種別") ||
        !appendDecodedQzssBits(messages, frame, "SDM/SV1-SV9", 15, 9) ||
        !appendDecodedQzssNumber(messages, "SV/受信衛星", satelliteId) ||
        !appendDecodedQzssValue(messages, "A1/通報種別", messageType, messageTypes[messageType]) ||
        !appendDecodedQzssValue(messages, "A2/地域", country, countryText) ||
        !appendDecodedQzssValue(messages, "A3/配信元", provider, providerText) ||
        !appendDecodedQzssValue(messages, "A4/災害", hazard, dcxHazardText(hazard)) ||
        !appendDecodedQzssValue(messages, "A5/重大度", severity, severities[severity]) ||
        !appendDecodedQzssValue(messages, "A6/発生週", onsetWeek, onsetWeek ? "次週" : "今週") ||
        !appendDecodedQzssValue(messages, "A7/週内時刻", onsetTimeOfWeek, a7Description) ||
        !appendDecodedQzssValue(messages, "A8/継続時間", duration, durations[duration]) ||
        !appendDecodedQzssValue(messages, "A9/指示ライブラリ", library, libraries[library]) ||
        !appendDecodedQzssValue(messages, "A10/ライブラリ版", libraryVersion, libraryVersionText))
        return false;
    for (uint8_t i = 0; i < 9; ++i) {
        const uint32_t satelliteField = getBits(frame, 15 + i, 1);
        const char *meaning = satelliteMaskType ? (satelliteField ? "MT44送信中" : "MT44送信停止")
                                               : (satelliteField ? "海外向け" : "日本向け");
        std::snprintf(line, sizeof(line), "SDM.SV%u", static_cast<unsigned>(i + 1));
        if (!appendDecodedQzssValue(messages, line, satelliteField, meaning))
            return false;
    }
    if (country == 111 && library == 1) {
        char instructionText[144];
        if (!formatDcxInstruction(instruction, instructionText, sizeof(instructionText)))
            return false;
        if (instructionText[0])
            std::snprintf(line, sizeof(line), "A11/行動指示=%s(%u)", instructionText,
                          static_cast<unsigned>(instruction));
        else
            std::snprintf(line, sizeof(line), "A11/行動指示=%u", static_cast<unsigned>(instruction));
        if (!appendDecodedQzssLine(messages, line))
            return false;
    } else if (library == 0) {
        const uint32_t actionA = instruction >> 5;
        const uint32_t actionB = instruction & 0x1F;
        if (!appendDecodedQzssNumber(messages, "A11/行動指示コード", instruction) ||
            !appendDecodedQzssValue(messages, "A11/行動指示A", actionA + 1, dcxInternationalInstructionA(actionA)) ||
            !appendDecodedQzssValue(messages, "A11/行動指示B", actionB + 1, dcxInternationalInstructionB(actionB)))
            return false;
    } else if (!japanJAlert) {
        std::snprintf(line, sizeof(line), "A11/行動指示=地域別定義・未対応(%u)",
                      static_cast<unsigned>(instruction));
        if (!appendDecodedQzssLine(messages, line))
            return false;
    }

    if (!appendDecodedQzssNumber(messages, "A12/楕円中心緯度コード", getBits(frame, 80, 16)) ||
        !appendDecodedQzssNumber(messages, "A13/楕円中心経度コード", getBits(frame, 96, 17)) ||
        !appendDecodedQzssNumber(messages, "A14/長半径コード", getBits(frame, 113, 5)) ||
        !appendDecodedQzssNumber(messages, "A15/短半径コード", getBits(frame, 118, 5)) ||
        !appendDecodedQzssNumber(messages, "A16/楕円方位コード", getBits(frame, 123, 6)) ||
        !appendDecodedQzssNumber(messages, "A17/個別設定主題", getBits(frame, 129, 2)) ||
        !appendDecodedQzssNumber(messages, "A18/個別設定", getBits(frame, 131, 15)))
        return false;
    if (japanJAlert) {
        if (!appendDecodedQzssLine(messages, "A12-A18/Jアラートでは未使用"))
            return false;
    } else if (!appendDcxEllipseDetails(messages, frame))
        return false;

    if (!japanJAlert &&
        (!appendDecodedQzssValue(messages, "A17/個別設定種別", getBits(frame, 129, 2),
                                 specificSettings[getBits(frame, 129, 2)]) ||
         !appendDcxA18Fields(messages, frame, hazard)))
        return false;

    if (country == 111 && (provider == 1 || provider == 4)) {
        const uint32_t targetCode = getBits(frame, 146, 16);
        char areaCode[32];
        std::snprintf(areaCode, sizeof(areaCode), "%05u", static_cast<unsigned>(targetCode));
        if (!appendDecodedQzssNumber(messages, "EX1/対象市区町村コード", targetCode))
            return false;
        std::snprintf(line, sizeof(line), "EX1/自治体コード=%s", areaCode);
        if (!appendDecodedQzssLine(messages, line))
            return false;
        if (provider == 1) {
            if (!appendDecodedQzssBits(messages, frame, "EX2-EX7/Lアラート未使用ビット", 162, 52))
                return false;
        } else {
            const uint32_t evacuateDirection = getBits(frame, 162, 1);
            const uint32_t extraLatitude = getBits(frame, 163, 17);
            const uint32_t extraLongitude = getBits(frame, 180, 17);
            const uint32_t extraMajor = getBits(frame, 197, 5);
            const uint32_t extraMinor = getBits(frame, 202, 5);
            const uint32_t extraAzimuth = getBits(frame, 207, 7);
            const int32_t extraLatMilliDegrees =
                -90000 + static_cast<int32_t>((static_cast<uint64_t>(extraLatitude) * 180000 + 65535) / 131071);
            const int32_t extraLonMilliDegrees =
                45000 + static_cast<int32_t>((static_cast<uint64_t>(extraLongitude) * 180000 + 65535) / 131071);
            const int32_t extraAzimuthCentiDegrees = -9000 + static_cast<int32_t>((extraAzimuth * 18000 + 64) / 128);
            if (!appendDecodedQzssValue(messages, "EX2/避難方向", evacuateDirection,
                                        evacuateDirection ? "追加対象範囲へ" : "追加対象範囲から離れる") ||
                !appendDecodedQzssNumber(messages, "EX3/追加楕円中心緯度コード", extraLatitude) ||
                !appendDecodedQzssFixed(messages, "EX3/追加楕円中心緯度", extraLatMilliDegrees, 3, "°") ||
                !appendDecodedQzssNumber(messages, "EX4/追加楕円中心経度コード", extraLongitude) ||
                !appendDecodedQzssFixed(messages, "EX4/追加楕円中心経度", extraLonMilliDegrees, 3, "°") ||
                !appendDecodedQzssNumber(messages, "EX5/追加楕円長半径コード", extraMajor) ||
                !appendDecodedQzssFixed(messages, "EX5/追加楕円長半径",
                                        static_cast<int32_t>(dcxEllipseRadiusMilliKm(extraMajor)), 3, "km") ||
                !appendDecodedQzssNumber(messages, "EX6/追加楕円短半径コード", extraMinor) ||
                !appendDecodedQzssFixed(messages, "EX6/追加楕円短半径",
                                        static_cast<int32_t>(dcxEllipseRadiusMilliKm(extraMinor)), 3, "km") ||
                !appendDecodedQzssNumber(messages, "EX7/追加楕円方位コード", extraAzimuth) ||
                !appendDecodedQzssFixed(messages, "EX7/追加楕円方位", extraAzimuthCentiDegrees, 2, "°"))
                return false;
        }
    } else if (country == 111 && (provider == 2 || provider == 3)) {
        if (!appendDecodedQzssValue(messages, "EX8/対象地域コード種別", getBits(frame, 146, 1),
                                    getBits(frame, 146, 1) ? "市区町村" : "都道府県") ||
            !appendDecodedQzssHex64(messages, "EX9/対象地域コード一覧64ビット", getBits64(frame, 147, 64)) ||
            !appendDecodedQzssNumber(messages, "EX10/予約", getBits(frame, 211, 3)))
            return false;
    } else if (!appendDecodedQzssBits(messages, frame, "EX11/未対応の国外組織固有情報68ビット", 146, 68)) {
        return false;
    }

    return appendDecodedQzssNumber(messages, "Vn/仕様バージョン", getBits(frame, 214, 6)) &&
           appendDecodedQzssNumber(messages, "予約", getBits(frame, 220, 6)) &&
           appendDecodedQzssHex(messages, "CRC24Q", getBits(frame, 226, 24), 6);
}

bool isDcxNullMessage(const uint8_t *frame)
{
    return getBits(frame, 24, 2) == 0 && getBits(frame, 26, 9) == 111 && getBits64(frame, 35, 64) == 0 &&
           getBits64(frame, 99, 47) == 0 && getBits64(frame, 146, 64) == 0 && getBits(frame, 210, 10) == 0;
}

void publishFrame(const uint8_t *payload)
{
    if (payload[0] == 5) {
        const uint8_t svId = payload[1];
        const uint8_t sigId = payload[2];
        if (sigId < MAX_SIGNAL_ID)
            ++qzssSfrbxBySignalId[sigId];
        else
            ++qzssSfrbxUnknownSignalIds;

        if (svId > 0 && svId <= MAX_MICHIBIKI_SV_ID && sigId <= QZSS_L1S_SIG_ID)
            ++qzssSfrbxBySatelliteSignal[svId - 1][sigId];
    } else {
        ++nonQzssSfrbxMessages;
    }

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

    ++l1sMessageTypeCounts[messageType];
    if (messageType == 63)
        return;
    if (messageType != 43 && messageType != 44)
        return;

    if constexpr (GPS_DEBUG) {
        static constexpr char hexDigits[] = "0123456789ABCDEF";
        char rawFrameHex[sizeof(frame) * 3] = {};
        for (size_t i = 0; i < sizeof(frame); ++i) {
            const size_t position = i * 3;
            rawFrameHex[position] = hexDigits[frame[i] >> 4];
            rawFrameHex[position + 1] = hexDigits[frame[i] & 0x0F];
            if (i + 1 < sizeof(frame))
                rawFrameHex[position + 2] = ' ';
        }
        LOG_INFO("QZSS MT%u raw: %s", static_cast<unsigned>(messageType), rawFrameHex);
    }

    lastMt43Or44Ms = now;
    hasMt43Or44 = true;
    if (messageType == 44 && isDcxNullMessage(frame))
        return;

    const bool canPublish = channelIndex != NO_CHANNEL && router && service;
    if (canPublish && isDuplicateMtFrame(frame)) {
        LOG_DEBUG_GPS("QZSS MT%u duplicate suppressed", static_cast<unsigned>(messageType));
        return;
    }

    // GPS parsing is single-threaded; keep this multi-kilobyte buffer off the loop task stack.
    static DecodedQzssMessages messages;
    if (GPS_DEBUG && messageType == 44 && formatDcxAllFields(frame, svId, messages)) {
        for (uint8_t i = 0; i < messages.count; ++i)
            LOG_INFO("[GPS] QZSS MT44 decoded: '%s'", messages.text[i]);
    }
    if (!canPublish)
        return;

    if (!(messageType == 43 ? formatDcrMessage(frame, messages) : formatDcxChatMessage(frame, messages)))
        return;

    for (uint8_t i = 0; i < messages.count; ++i) {
        meshtastic_MeshPacket *packet = router->allocForSending();
        if (!packet)
            continue;
        packet->channel = channelIndex;
        packet->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
        packet->decoded.payload.size = messages.length[i];
        std::memcpy(packet->decoded.payload.bytes, messages.text[i], messages.length[i]);
        packet->decoded.want_response = false;
        if (satelliteRegistered)
            packet->from = MICHIBIKI_NODE_NUM_BASE + svId;

#if HAS_SCREEN || defined(MESHTASTIC_INCLUDE_NICHE_GRAPHICS)
        messageStore.tryAddFromPacket(*packet);
#endif
        service->sendToPhone(packet);
    }
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
        ++navSatReports;

        uint16_t satellites[MAX_MICHIBIKI_SV_ID] = {};
        uint8_t diagnosticCno[MAX_MICHIBIKI_SV_ID] = {};
        uint8_t qzssSatellitesInReport = 0;
        for (uint8_t i = 0; i < numSvs; ++i) {
            const uint8_t *entry = parser.payload + 8 + static_cast<uint16_t>(i) * 12;
            if (entry[0] != 5 || entry[1] == 0 || entry[1] > MAX_MICHIBIKI_SV_ID)
                continue;

            const uint8_t svId = entry[1];
            if (entry[2] > 0) {
                ++qzssSatellitesInReport;
                ++navSatQzssSatelliteObservations;
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
        if (qzssSatellitesInReport)
            ++navSatReportsWithQzss;
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
             "PAB=%lu/%lu CRC=%lu/%lu",
             static_cast<unsigned long>(sfrbxMessages), static_cast<unsigned long>(sfrbxChecksumErrors),
             static_cast<unsigned long>(sfrbxOversizeMessages), static_cast<unsigned long>(sfrbxLengthErrors),
             static_cast<unsigned long>(nonL1sSfrbxMessages), static_cast<unsigned long>(l1sSfrbxMessages),
             static_cast<unsigned long>(l1sWordCountErrors), static_cast<unsigned long>(l1sPreambleMatches),
             static_cast<unsigned long>(l1sPreambleRejects), static_cast<unsigned long>(l1sCrcMatches),
             static_cast<unsigned long>(l1sCrcRejects));

    char signalSummary[256] = {};
    size_t signalPosition = 0;
    for (uint8_t sigId = 0; sigId < MAX_SIGNAL_ID; ++sigId) {
        const uint32_t count = qzssSfrbxBySignalId[sigId];
        if (!count)
            continue;
        const int length = std::snprintf(signalSummary + signalPosition, sizeof(signalSummary) - signalPosition,
                                         "%s%u=%lu", signalPosition ? "," : "", static_cast<unsigned>(sigId),
                                         static_cast<unsigned long>(count));
        if (length < 0 || static_cast<size_t>(length) >= sizeof(signalSummary) - signalPosition)
            break;
        signalPosition += static_cast<size_t>(length);
    }
    LOG_INFO("QZSS SFRBX sigId/30s (0=L1CA,1=L1S): %s unknown=%lu nonQZSS=%lu",
             signalPosition ? signalSummary : "none",
             static_cast<unsigned long>(qzssSfrbxUnknownSignalIds),
             static_cast<unsigned long>(nonQzssSfrbxMessages));

    char navSatAge[16] = "never";
    if (hasSatelliteReport.load())
        std::snprintf(navSatAge, sizeof(navSatAge), "%lus",
                      static_cast<unsigned long>((now - lastSatelliteReportMs.load()) / 1000));
    LOG_INFO("QZSS NAV-SAT/30s: reports=%lu withQZSS=%lu svObs=%lu age=%s",
             static_cast<unsigned long>(navSatReports), static_cast<unsigned long>(navSatReportsWithQzss),
             static_cast<unsigned long>(navSatQzssSatelliteObservations), navSatAge);

    char messageTypeSummary[128] = {};
    size_t messageTypePosition = 0;
    for (uint8_t messageType = 0; messageType < 64; ++messageType) {
        const uint32_t count = l1sMessageTypeCounts[messageType];
        if (!count)
            continue;
        char entry[24];
        const int entryLength = std::snprintf(entry, sizeof(entry), "%u=%lu", static_cast<unsigned>(messageType),
                                              static_cast<unsigned long>(count));
        if (entryLength < 0 || static_cast<size_t>(entryLength) >= sizeof(entry))
            continue;
        const size_t separatorLength = messageTypePosition ? 1 : 0;
        if (messageTypePosition + separatorLength + static_cast<size_t>(entryLength) >= sizeof(messageTypeSummary)) {
            LOG_INFO("QZSS MT/30s: %s", messageTypeSummary);
            messageTypePosition = 0;
            messageTypeSummary[0] = '\0';
        }
        if (messageTypePosition)
            messageTypeSummary[messageTypePosition++] = ',';
        std::memcpy(messageTypeSummary + messageTypePosition, entry, static_cast<size_t>(entryLength));
        messageTypePosition += static_cast<size_t>(entryLength);
        messageTypeSummary[messageTypePosition] = '\0';
    }
    LOG_INFO("QZSS MT/30s: %s", messageTypePosition ? messageTypeSummary : "none");

    char satelliteSummary[768] = {};
    size_t position = 0;
    const bool satelliteReportFresh = hasSatelliteReport.load() &&
                                      !Throttle::hasElapsed(lastSatelliteReportMs.load(), 5000);
    for (uint8_t i = 0; i < MAX_MICHIBIKI_SV_ID; ++i) {
        const uint8_t cno = satelliteReportFresh ? diagnosticQzssCno[i].load() : 0;
        const uint32_t l1caMessages = qzssSfrbxBySatelliteSignal[i][QZSS_L1CA_SIG_ID];
        const uint32_t l1sMessages = qzssSfrbxBySatelliteSignal[i][QZSS_L1S_SIG_ID];
        const uint32_t validFrames = l1sFramesBySatellite[i];
        if (!cno && !l1caMessages && !l1sMessages && !validFrames)
            continue;
        char cnoText[4] = "-";
        if (cno)
            std::snprintf(cnoText, sizeof(cnoText), "%u", static_cast<unsigned>(cno));
        const uint8_t svId = i + 1;
        const int length = std::snprintf(satelliteSummary + position, sizeof(satelliteSummary) - position,
                                         "%sSV%u C/N0=%s L1CA=%lu L1S=%lu ok=%lu", position ? ", " : "",
                                         static_cast<unsigned>(192 + svId), cnoText,
                                         static_cast<unsigned long>(l1caMessages),
                                         static_cast<unsigned long>(l1sMessages),
                                         static_cast<unsigned long>(validFrames));
        if (length < 0 || static_cast<size_t>(length) >= sizeof(satelliteSummary) - position)
            break;
        position += static_cast<size_t>(length);
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
    std::memset(l1sMessageTypeCounts, 0, sizeof(l1sMessageTypeCounts));
    std::memset(l1sFramesBySatellite, 0, sizeof(l1sFramesBySatellite));
    std::memset(qzssSfrbxBySignalId, 0, sizeof(qzssSfrbxBySignalId));
    std::memset(qzssSfrbxBySatelliteSignal, 0, sizeof(qzssSfrbxBySatelliteSignal));
    qzssSfrbxUnknownSignalIds = 0;
    nonQzssSfrbxMessages = 0;
    navSatReports = 0;
    navSatReportsWithQzss = 0;
    navSatQzssSatelliteObservations = 0;
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

bool beginReceiveBatch()
{
    if (parserResetPending.exchange(false))
        parser.reset();
    return enabled.load();
}

bool consumeByte(uint8_t byte)
{
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
        } else if (enabled.load()) {
            processUbxMessage();
        }
        parser.reset();
        return true;
    }
    parser.reset();
    return false;
}
} // namespace QzssDcr
#endif
