#pragma once

#include <cstdint>

extern "C" {

int32_t bobMain();

void bobStart();

void bobRuntimeWrite(int32_t stream, const uint8_t* bytes, uint64_t count);

}
