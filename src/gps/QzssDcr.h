#pragma once

#include <cstdint>

namespace QzssDcr
{
inline constexpr char SETTINGS_FILE_NAME[] = "/prefs/qzss_dcr.dat";

bool setupChannel();
bool isEnabled();
bool setEnabled(bool enabled);
void reset();
bool consumeByte(uint8_t byte);
} // namespace QzssDcr
