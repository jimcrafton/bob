#pragma once

#include <cstdint>

// UTF-8 and extended grapheme clusters (UAX #29, Unicode 16.0). Freestanding:
// no CRT or OS calls, so the test suite compiles it too (unicodeTests.cpp).
namespace bob::unicode {

enum class GraphemeBreak : uint8_t {
    other = 0,
    cr = 1,
    lf = 2,
    control = 3,
    extend = 4,
    zwj = 5,
    regionalIndicator = 6,
    prepend = 7,
    spacingMark = 8,
    l = 9,
    v = 10,
    t = 11,
    lv = 12,
    lvt = 13,
};

GraphemeBreak graphemeBreak(uint32_t codepoint);
bool isExtendedPictographic(uint32_t codepoint);

// Decodes the codepoint at `offset` and advances it. Invalid sequences decode as
// U+FFFD and advance by one byte (strings are valid UTF-8, so this only matters
// for checking).
uint32_t decodeUtf8(const uint8_t* bytes, uint64_t count, uint64_t& offset);
// The length of the valid UTF-8 sequence at `offset`, or 0.
uint32_t validSequenceLength(const uint8_t* bytes, uint64_t count, uint64_t offset);
bool isValidUtf8(const uint8_t* bytes, uint64_t count);
// Writes the UTF-8 encoding of `codepoint` (a scalar value); returns its length.
uint32_t encodeUtf8(uint32_t codepoint, uint8_t* out);

// The offset where the grapheme cluster starting at `offset` ends.
uint64_t nextGraphemeBoundary(const uint8_t* bytes, uint64_t count, uint64_t offset);

}
