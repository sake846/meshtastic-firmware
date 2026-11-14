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
#include "mesh/Channels.h"
#include "mesh/MeshService.h"
#include "mesh/NodeDB.h"
#include "mesh/Router.h"
#include "mesh/Throttle.h"
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
constexpr uint16_t MAX_UBX_PAYLOAD = 64;
constexpr uint32_t CRC24Q_POLY = 0x864CFB;
constexpr uint32_t CRC24_MASK = 0xFFFFFF;
constexpr uint8_t L1S_PREAMBLES[] = {0x53, 0x9A, 0xC6};
constexpr uint32_t SETTINGS_MAGIC = 0x515A4C31;
constexpr uint8_t SETTINGS_VERSION = 1;
constexpr uint32_t CAPTURE_TIMEOUT_MS = 15000;

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

constexpr NodeNum MICHIBIKI_NODE_NUM_BASE = 0x515A5300; // QZS prefix, with the UBX svId in the low byte.
constexpr uint8_t MAX_MICHIBIKI_SV_ID = 10;

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

void publishFrame(const uint8_t *payload)
{
    if (payload[0] != 5 || payload[2] != 1 || payload[4] != 8)
        return;
    const uint8_t svId = payload[1];
    const bool satelliteRegistered = registerMichibikiSatellite(svId);

    uint8_t frame[32];
    for (uint8_t word = 0; word < 8; ++word) {
        const uint8_t offset = 8 + word * 4;
        frame[word * 4] = payload[offset + 3];
        frame[word * 4 + 1] = payload[offset + 2];
        frame[word * 4 + 2] = payload[offset + 1];
        frame[word * 4 + 3] = payload[offset];
    }

    const uint8_t preamble = frame[0];
    if (preamble != L1S_PREAMBLES[0] && preamble != L1S_PREAMBLES[1] && preamble != L1S_PREAMBLES[2])
        return;
    if (crc24q(frame) != getBits(frame, 226, 24))
        return;

    lastCaptureSvId.store(svId);
    lastCaptureMs.store(Time::getMillis());
    hasCapturedSatellite.store(true);

    const uint8_t messageType = static_cast<uint8_t>(getBits(frame, 8, 6));
    if (messageType != 43 && messageType != 44)
        return;

    frame[31] &= 0xC0;
    if (seenRecently(frameHash(frame)) || channelIndex == NO_CHANNEL || !router || !service)
        return;

    char message[160];
    int length;
    if (messageType == 43) {
        const uint8_t reportClass = static_cast<uint8_t>(getBits(frame, 14, 3));
        const uint8_t disasterCategory = static_cast<uint8_t>(getBits(frame, 17, 4));
        length = std::snprintf(message, sizeof(message), "みちびき 災危通報 Rc=%u Dc=%u\n", static_cast<unsigned>(reportClass),
                               static_cast<unsigned>(disasterCategory));
    } else {
        length = std::snprintf(message, sizeof(message), "みちびき DCX\n");
    }
    if (length < 0 || static_cast<size_t>(length) >= sizeof(message))
        return;

    size_t position = static_cast<size_t>(length);
    for (uint8_t i = 0; i < 32; ++i) {
        const int written = std::snprintf(message + position, sizeof(message) - position, "%02X", frame[i]);
        if (written != 2)
            return;
        position += 2;
    }

    meshtastic_MeshPacket *packet = router->allocForSending();
    if (!packet)
        return;
    packet->channel = channelIndex;
    packet->decoded.portnum = meshtastic_PortNum_TEXT_MESSAGE_APP;
    packet->decoded.payload.size = static_cast<uint16_t>(position);
    std::memcpy(packet->decoded.payload.bytes, message, position);
    packet->decoded.want_response = false;
    if (satelliteRegistered)
        packet->from = MICHIBIKI_NODE_NUM_BASE + svId;

#if HAS_SCREEN || defined(MESHTASTIC_INCLUDE_NICHE_GRAPHICS)
    messageStore.tryAddFromPacket(*packet);
#endif
    service->sendToPhone(packet);
}

void processUbxMessage()
{
    if (parser.messageClass == 0x02 && parser.messageId == 0x13 && parser.length == 40)
        publishFrame(parser.payload);
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
        if (channel.role != meshtastic_Channel_Role_DISABLED || channel.has_settings)
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

bool getCapturedSatellite(uint8_t &svId)
{
    const uint32_t lastFrameMs = lastCaptureMs.load();
    if (!hasCapturedSatellite.load() || Throttle::hasElapsed(lastFrameMs, CAPTURE_TIMEOUT_MS))
        return false;

    svId = lastCaptureSvId.load();
    return true;
}

void setUbloxM10Detected(bool detected)
{
    m10Detected.store(detected);
    if (!detected)
        hasCapturedSatellite.store(false);
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
    parserResetPending.store(true);
    return true;
}

void reset()
{
    parser.reset();
    hasCapturedSatellite.store(false);
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
        if (!parser.payloadTooLarge && parser.receivedChecksumA == parser.checksumA && byte == parser.checksumB)
            processUbxMessage();
        parser.reset();
        return true;
    }
    parser.reset();
    return false;
}
} // namespace QzssDcr
#endif
