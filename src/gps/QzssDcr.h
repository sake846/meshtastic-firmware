#pragma once

#include <cstdint>

namespace QzssDcr
{
inline constexpr char SETTINGS_FILE_NAME[] = "/prefs/qzss_dcr.dat";
inline constexpr uint8_t MAX_QZSS_SATELLITES = 10;
inline constexpr uint32_t MICHIBIKI_NODE_NUM_BASE = 0x515A5300;

struct SatelliteStatus {
    uint8_t svId;
    uint8_t cno;
};

struct SatelliteReceptionStatus {
    uint8_t cno;
    uint8_t messageType;
    uint32_t lastL1SAgeSeconds;
    bool hasL1S;
};

bool setupChannel();
bool isEnabled();
bool isUbloxM10Detected();
bool getCapturedSatellite(uint8_t &svId, uint8_t *ageSeconds = nullptr);
bool getSatelliteStatus(SatelliteStatus *satellites, uint8_t &count);
bool getLastSatelliteCno(uint8_t svId, uint8_t &cno);
bool getSatelliteReceptionStatus(uint8_t svId, SatelliteReceptionStatus &status);
void logDiagnostics(const char *powerState);
void setUbloxM10Detected(bool detected);
bool setEnabled(bool enabled);
void setReceptionEnabled(bool enabled);
void reset();
bool consumeByte(uint8_t byte);
} // namespace QzssDcr
