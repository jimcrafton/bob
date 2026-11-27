#include "unicode.h"

namespace bob::unicode {

namespace {

struct UnicodeRange {
    uint32_t first;
    uint32_t last;
    uint8_t properties;
};

#include "unicodeTables.inc"

constexpr uint8_t pictographicBit = 0x10;

enum : uint8_t {
    incbNone = 0,
    incbLinker = 1,
    incbConsonant = 2,
    incbExtend = 3,
};

uint8_t properties(uint32_t codepoint)
{
    uint32_t low = 0;
    uint32_t high = unicodeRangeCount;
    while (low < high) {
        uint32_t middle = (low + high) / 2;
        const UnicodeRange& range = unicodeRanges[middle];
        if (codepoint < range.first) {
            high = middle;
        }
        else if (codepoint > range.last) {
            low = middle + 1;
        }
        else {
            return range.properties;
        }
    }
    return 0;
}

GraphemeBreak breakOf(uint8_t props)
{
    return static_cast<GraphemeBreak>(props & 0x0F);
}

uint8_t incbOf(uint8_t props)
{
    return static_cast<uint8_t>((props >> 5) & 0x03);
}

bool isControlLike(GraphemeBreak b)
{
    return b == GraphemeBreak::control || b == GraphemeBreak::cr || b == GraphemeBreak::lf;
}

}

GraphemeBreak graphemeBreak(uint32_t codepoint)
{
    return breakOf(properties(codepoint));
}

bool isExtendedPictographic(uint32_t codepoint)
{
    return (properties(codepoint) & pictographicBit) != 0;
}

uint32_t validSequenceLength(const uint8_t* bytes, uint64_t count, uint64_t offset)
{
    uint8_t lead = bytes[offset];
    if (lead < 0x80) {
        return 1;
    }
    uint32_t length = 0;
    uint32_t codepoint = 0;
    uint32_t minimum = 0;
    if ((lead & 0xE0) == 0xC0) {
        length = 2;
        codepoint = lead & 0x1F;
        minimum = 0x80;
    }
    else if ((lead & 0xF0) == 0xE0) {
        length = 3;
        codepoint = lead & 0x0F;
        minimum = 0x800;
    }
    else if ((lead & 0xF8) == 0xF0) {
        length = 4;
        codepoint = lead & 0x07;
        minimum = 0x10000;
    }
    else {
        return 0;
    }
    if (offset + length > count) {
        return 0;
    }
    for (uint32_t i = 1; i < length; ++i) {
        uint8_t b = bytes[offset + i];
        if ((b & 0xC0) != 0x80) {
            return 0;
        }
        codepoint = (codepoint << 6) | (b & 0x3F);
    }
    if (codepoint < minimum || codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
        return 0;
    }
    return length;
}

uint32_t decodeUtf8(const uint8_t* bytes, uint64_t count, uint64_t& offset)
{
    uint32_t length = validSequenceLength(bytes, count, offset);
    if (length == 0) {
        ++offset;
        return 0xFFFD;
    }
    uint8_t lead = bytes[offset];
    uint32_t codepoint = length == 1 ? lead : length == 2 ? (lead & 0x1Fu) : length == 3 ? (lead & 0x0Fu) : (lead & 0x07u);
    for (uint32_t i = 1; i < length; ++i) {
        codepoint = (codepoint << 6) | (bytes[offset + i] & 0x3Fu);
    }
    offset += length;
    return codepoint;
}

bool isValidUtf8(const uint8_t* bytes, uint64_t count)
{
    uint64_t offset = 0;
    while (offset < count) {
        uint32_t length = validSequenceLength(bytes, count, offset);
        if (length == 0) {
            return false;
        }
        offset += length;
    }
    return true;
}

uint32_t encodeUtf8(uint32_t codepoint, uint8_t* out)
{
    if (codepoint < 0x80) {
        out[0] = static_cast<uint8_t>(codepoint);
        return 1;
    }
    if (codepoint < 0x800) {
        out[0] = static_cast<uint8_t>(0xC0 | (codepoint >> 6));
        out[1] = static_cast<uint8_t>(0x80 | (codepoint & 0x3F));
        return 2;
    }
    if (codepoint < 0x10000) {
        out[0] = static_cast<uint8_t>(0xE0 | (codepoint >> 12));
        out[1] = static_cast<uint8_t>(0x80 | ((codepoint >> 6) & 0x3F));
        out[2] = static_cast<uint8_t>(0x80 | (codepoint & 0x3F));
        return 3;
    }
    out[0] = static_cast<uint8_t>(0xF0 | (codepoint >> 18));
    out[1] = static_cast<uint8_t>(0x80 | ((codepoint >> 12) & 0x3F));
    out[2] = static_cast<uint8_t>(0x80 | ((codepoint >> 6) & 0x3F));
    out[3] = static_cast<uint8_t>(0x80 | (codepoint & 0x3F));
    return 4;
}

// UAX #29 rules GB3-GB13, applied left to right with the little state the
// context-dependent rules need (GB9c, GB11, GB12/13).
uint64_t nextGraphemeBoundary(const uint8_t* bytes, uint64_t count, uint64_t offset)
{
    if (offset >= count) {
        return count;
    }
    uint8_t prevProps = properties(decodeUtf8(bytes, count, offset));
    GraphemeBreak prev = breakOf(prevProps);
    uint32_t regionalCount = prev == GraphemeBreak::regionalIndicator ? 1 : 0;
    // GB11: 0 none, 1 after ExtPict Extend*, 2 after ExtPict Extend* ZWJ.
    int emoji = (prevProps & pictographicBit) != 0 ? 1 : 0;
    // GB9c: 0 none, 1 after Consonant [Extend Linker]*, 2 once a Linker was seen.
    int conjunct = incbOf(prevProps) == incbConsonant ? 1 : 0;

    while (offset < count) {
        uint64_t nextOffset = offset;
        uint8_t nextProps = properties(decodeUtf8(bytes, count, nextOffset));
        GraphemeBreak next = breakOf(nextProps);
        uint8_t nextIncb = incbOf(nextProps);
        bool join = false;
        if (prev == GraphemeBreak::cr && next == GraphemeBreak::lf) {
            join = true; // GB3
        }
        else if (isControlLike(prev) || isControlLike(next)) {
            join = false; // GB4, GB5
        }
        else if (prev == GraphemeBreak::l &&
                 (next == GraphemeBreak::l || next == GraphemeBreak::v || next == GraphemeBreak::lv || next == GraphemeBreak::lvt)) {
            join = true; // GB6
        }
        else if ((prev == GraphemeBreak::lv || prev == GraphemeBreak::v) && (next == GraphemeBreak::v || next == GraphemeBreak::t)) {
            join = true; // GB7
        }
        else if ((prev == GraphemeBreak::lvt || prev == GraphemeBreak::t) && next == GraphemeBreak::t) {
            join = true; // GB8
        }
        else if (next == GraphemeBreak::extend || next == GraphemeBreak::zwj || next == GraphemeBreak::spacingMark ||
                 prev == GraphemeBreak::prepend) {
            join = true; // GB9, GB9a, GB9b
        }
        else if (conjunct == 2 && nextIncb == incbConsonant) {
            join = true; // GB9c
        }
        else if (emoji == 2 && (nextProps & pictographicBit) != 0) {
            join = true; // GB11
        }
        else if (prev == GraphemeBreak::regionalIndicator && next == GraphemeBreak::regionalIndicator && regionalCount % 2 == 1) {
            join = true; // GB12, GB13
        }
        if (!join) {
            return offset;
        }

        regionalCount = next == GraphemeBreak::regionalIndicator ? regionalCount + 1 : 0;
        if ((nextProps & pictographicBit) != 0) {
            emoji = 1;
        }
        else if (emoji == 1 && next == GraphemeBreak::extend) {
            emoji = 1;
        }
        else if (emoji == 1 && next == GraphemeBreak::zwj) {
            emoji = 2;
        }
        else {
            emoji = 0;
        }
        if (nextIncb == incbConsonant) {
            conjunct = 1;
        }
        else if (conjunct != 0 && nextIncb == incbLinker) {
            conjunct = 2;
        }
        else if (conjunct != 0 && nextIncb == incbExtend) {
            // stays
        }
        else {
            conjunct = 0;
        }
        prev = next;
        offset = nextOffset;
    }
    return count;
}

}
