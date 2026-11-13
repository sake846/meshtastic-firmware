#include "configuration.h"
#if defined(HAS_QZSS_DCR)
#include "MessageStore.h"
#include "QzssDcr.h"
#include "mesh/Channels.h"
#include "mesh/MeshService.h"
#include "mesh/NodeDB.h"
#include "mesh/Router.h"
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

uint8_t channelIndex = NO_CHANNEL;
uint32_t recentFrames[16] = {};
uint8_t recentFrameIndex = 0;

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

    messageStore.tryAddFromPacket(*packet);
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
    if (channelFile.channels_count == 0)
        return false;

    for (ChannelIndex i = 0; i < channelFile.channels_count; ++i) {
        meshtastic_Channel &channel = channels.getByIndex(i);
        if (channel.has_settings && std::strcmp(channel.settings.name, CHANNEL_NAME) == 0) {
            channelIndex = i;
            if (channel.role == meshtastic_Channel_Role_DISABLED) {
                channel.role = meshtastic_Channel_Role_SECONDARY;
                channels.onConfigChanged();
                nodeDB->saveChannelsToDisk();
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
        nodeDB->saveChannelsToDisk();
        return true;
    }

    LOG_WARN("No unused channel for QZSS DCR messages");
    return false;
}

void reset()
{
    parser.reset();
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
