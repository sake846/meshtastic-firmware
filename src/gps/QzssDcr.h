#pragma once

#include <cstdint>

namespace QzssDcr
{
inline constexpr char SETTINGS_FILE_NAME[] = "/prefs/qzss_dcr.dat";
inline constexpr uint8_t MAX_QZSS_SATELLITES = 10;

struct SatelliteStatus {
    uint8_t svId;
    uint8_t cno;
};

bool setupChannel();
bool isEnabled();
bool isUbloxM10Detected();
bool getCapturedSatellite(uint8_t &svId, uint8_t *ageSeconds = nullptr);
bool getSatelliteStatus(SatelliteStatus *satellites, uint8_t &count);
void setUbloxM10Detected(bool detected);
bool setEnabled(bool enabled);
void setReceptionEnabled(bool enabled);
void reset();
bool consumeByte(uint8_t byte);
} // namespace QzssDcr
