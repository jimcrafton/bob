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

inline constexpr const char* trapSymbol = "bobRuntimeTrap";

// Process exit code for every runtime trap (the same code Rust uses for panics);
// the reason is printed to stderr.
inline constexpr int32_t trapExitCode = 101;

enum class TrapReason : int32_t {
    integerOverflow = 1,
    divideByZero = 2,
    conversionOutOfRange = 3,
    shiftOutOfRange = 4,
    unreachable = 5,
    // phase 5
    valueOutOfRange = 6,   // assigning into a subrange
    ordinalOutOfRange = 7, // `succ` past the last member, `pred` before the first
    indexOutOfRange = 8,
    keyNotFound = 9,
    invalidUtf8 = 10,
};

}
