#pragma once

#include <cstdint>

namespace QzssDcr
{
inline constexpr char SETTINGS_FILE_NAME[] = "/prefs/qzss_dcr.dat";

bool setupChannel();
bool isEnabled();
bool isM10QDetected();
bool getCapturedSatellite(uint8_t &svId);
void setM10QDetected(bool detected);
bool setEnabled(bool enabled);
void setReceptionEnabled(bool enabled);
void reset();
bool consumeByte(uint8_t byte);
} // namespace QzssDcr
