#pragma once

#include <cstdint>

namespace bob::abi {

inline constexpr const char* entrySymbol = "bobStart";
inline constexpr const char* mainSymbol = "bobMain";
inline constexpr const char* runtimeLibraryName = "bobrt.lib";
inline constexpr const char* kernel32LibraryName = "kernel32.lib";

enum class StreamId : int32_t {
    out = 1,
    error = 2,
};

}
