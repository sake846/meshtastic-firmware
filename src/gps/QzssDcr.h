#pragma once

#include <cstdint>

namespace QzssDcr
{
bool setupChannel();
void reset();
bool consumeByte(uint8_t byte);
} // namespace QzssDcr
