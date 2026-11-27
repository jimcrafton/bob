// `string`: immutable UTF-8 buffers, shared by reference counting (so copies are
// free and there's nothing to copy on write). A null pointer is the empty string.

#include "abi.h"
#include "bobRuntime.h"
#include "objectModel.h"
#include "runtimeInternal.h"
#include "unicode.h"

extern "C" {
BobClass bobStringClass = {nullptr, "string", 0, nullptr, nullptr, 0, 0, nullptr};
}

namespace bob::runtime {

void copyBytes(void* dest, const void* src, uint64_t count)
{
    auto* d = static_cast<uint8_t*>(dest);
    const auto* s = static_cast<const uint8_t*>(src);
    for (uint64_t i = 0; i < count; ++i) {
        d[i] = s[i];
    }
}

bool sameBytes(const void* a, const void* b, uint64_t count)
{
    const auto* x = static_cast<const uint8_t*>(a);
    const auto* y = static_cast<const uint8_t*>(b);
    for (uint64_t i = 0; i < count; ++i) {
        if (x[i] != y[i]) {
            return false;
        }
    }
    return true;
}

const uint8_t* stringBytes(void* s)
{
    return s != nullptr ? static_cast<BobString*>(s)->bytes : nullptr;
}

uint64_t stringCount(void* s)
{
    return s != nullptr ? static_cast<BobString*>(s)->byteCount : 0;
}

// A string of `count` bytes to fill in; null when count is 0.
BobString* newString(uint64_t count)
{
    if (count == 0) {
        return nullptr;
    }
    auto* s = static_cast<BobString*>(bobAllocBuffer(&bobStringClass, sizeof(BobString) + count));
    s->byteCount = count;
    return s;
}

}

using namespace bob::runtime;

namespace {

const BobElementType stringElement = {sizeof(void*), retainPointer, releasePointer, 1, 0};
const BobElementType byteElement = {1, nullptr, nullptr, 0, 0};

int64_t find(void* s, void* t, uint64_t from)
{
    uint64_t n = stringCount(s);
    uint64_t m = stringCount(t);
    if (m == 0) {
        return static_cast<int64_t>(from);
    }
    for (uint64_t i = from; i + m <= n; ++i) {
        if (sameBytes(stringBytes(s) + i, stringBytes(t), m)) {
            return static_cast<int64_t>(i);
        }
    }
    return -1;
}

}

extern "C" {

void* bobStringFromBytes(const uint8_t* bytes, uint64_t count)
{
    BobString* s = newString(count);
    if (s != nullptr) {
        copyBytes(s->bytes, bytes, count);
    }
    return s;
}

void* bobStringConcat(void* a, void* b)
{
    uint64_t n = stringCount(a);
    uint64_t m = stringCount(b);
    if (m == 0 || n == 0) {
        void* only = m == 0 ? a : b;
        bobRetain(only);
        return only;
    }
    BobString* s = newString(n + m);
    copyBytes(s->bytes, stringBytes(a), n);
    copyBytes(s->bytes + n, stringBytes(b), m);
    return s;
}

// Byte-wise, which for UTF-8 is codepoint order.
int32_t bobStringCompare(void* a, void* b)
{
    if (a == b) {
        return 0;
    }
    uint64_t n = stringCount(a);
    uint64_t m = stringCount(b);
    uint64_t shorter = n < m ? n : m;
    const uint8_t* x = stringBytes(a);
    const uint8_t* y = stringBytes(b);
    for (uint64_t i = 0; i < shorter; ++i) {
        if (x[i] != y[i]) {
            return x[i] < y[i] ? -1 : 1;
        }
    }
    return n == m ? 0 : (n < m ? -1 : 1);
}

int64_t bobStringByteCount(void* s)
{
    return static_cast<int64_t>(stringCount(s));
}

int64_t bobStringCodepointCount(void* s)
{
    const uint8_t* bytes = stringBytes(s);
    uint64_t count = stringCount(s);
    int64_t codepoints = 0;
    for (uint64_t i = 0; i < count; ++i) {
        codepoints += (bytes[i] & 0xC0) != 0x80 ? 1 : 0;
    }
    return codepoints;
}

int64_t bobStringGraphemeCount(void* s)
{
    const uint8_t* bytes = stringBytes(s);
    uint64_t count = stringCount(s);
    int64_t graphemes = 0;
    for (uint64_t offset = 0; offset < count; offset = bob::unicode::nextGraphemeBoundary(bytes, count, offset)) {
        ++graphemes;
    }
    return graphemes;
}

uint8_t bobStringByteAt(void* s, int64_t index)
{
    if (index < 0 || static_cast<uint64_t>(index) >= stringCount(s)) {
        bobRuntimeTrap(static_cast<int32_t>(bob::abi::TrapReason::indexOutOfRange));
    }
    return stringBytes(s)[index];
}

uint32_t bobStringDecodeAt(void* s, int64_t* offset)
{
    uint64_t at = static_cast<uint64_t>(*offset);
    uint32_t codepoint = bob::unicode::decodeUtf8(stringBytes(s), stringCount(s), at);
    *offset = static_cast<int64_t>(at);
    return codepoint;
}

int64_t bobStringNextGrapheme(void* s, int64_t offset)
{
    return static_cast<int64_t>(bob::unicode::nextGraphemeBoundary(stringBytes(s), stringCount(s), static_cast<uint64_t>(offset)));
}

void* bobStringSlice(void* s, int64_t from, int64_t to)
{
    if (from == 0 && static_cast<uint64_t>(to) == stringCount(s)) {
        bobRetain(s);
        return s;
    }
    return bobStringFromBytes(stringBytes(s) + from, static_cast<uint64_t>(to - from));
}

bool bobStringPredicate(void* s, void* t, int32_t kind)
{
    uint64_t n = stringCount(s);
    uint64_t m = stringCount(t);
    switch (kind) {
    case 0:
        return m <= n && sameBytes(stringBytes(s), stringBytes(t), m);
    case 1:
        return m <= n && sameBytes(stringBytes(s) + (n - m), stringBytes(t), m);
    default:
        return find(s, t, 0) >= 0;
    }
}

void* bobStringSplit(void* s, void* separator)
{
    void* parts = nullptr;
    uint64_t n = stringCount(s);
    if (n == 0) {
        return parts;
    }
    uint64_t m = stringCount(separator);
    uint64_t start = 0;
    while (true) {
        int64_t at = m == 0 ? -1 : find(s, separator, start);
        uint64_t end = at < 0 ? n : static_cast<uint64_t>(at);
        void* part = bobStringFromBytes(stringBytes(s) + start, end - start);
        bobArrayPush(&parts, &part, &stringElement);
        if (at < 0) {
            return parts;
        }
        start = end + m;
    }
}

void* bobStringJoin(void* separator, void* strings)
{
    auto* array = static_cast<BobArray*>(strings);
    if (array == nullptr || array->count == 0) {
        return nullptr;
    }
    auto** items = reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(array) + bob::abi::arrayDataOffset);
    uint64_t total = stringCount(separator) * (array->count - 1);
    for (uint64_t i = 0; i < array->count; ++i) {
        total += stringCount(items[i]);
    }
    BobString* result = newString(total);
    if (result == nullptr) {
        return nullptr;
    }
    uint64_t at = 0;
    for (uint64_t i = 0; i < array->count; ++i) {
        if (i > 0) {
            copyBytes(result->bytes + at, stringBytes(separator), stringCount(separator));
            at += stringCount(separator);
        }
        copyBytes(result->bytes + at, stringBytes(items[i]), stringCount(items[i]));
        at += stringCount(items[i]);
    }
    return result;
}

void* bobStringFromUtf8(void* byteArray, int32_t lossy, int32_t* ok)
{
    auto* array = static_cast<BobArray*>(byteArray);
    uint64_t count = array != nullptr ? array->count : 0;
    const uint8_t* bytes = array != nullptr ? reinterpret_cast<uint8_t*>(array) + bob::abi::arrayDataOffset : nullptr;
    *ok = 1;
    if (bob::unicode::isValidUtf8(bytes, count)) {
        return bobStringFromBytes(bytes, count);
    }
    if (lossy == 0) {
        *ok = 0;
        return nullptr;
    }
    // Each invalid byte becomes U+FFFD (3 bytes).
    uint64_t length = 0;
    for (uint64_t offset = 0; offset < count;) {
        uint32_t valid = bob::unicode::validSequenceLength(bytes, count, offset);
        length += valid == 0 ? 3 : valid;
        offset += valid == 0 ? 1 : valid;
    }
    BobString* s = newString(length);
    uint64_t at = 0;
    for (uint64_t offset = 0; offset < count;) {
        uint32_t valid = bob::unicode::validSequenceLength(bytes, count, offset);
        if (valid == 0) {
            at += bob::unicode::encodeUtf8(0xFFFD, s->bytes + at);
            ++offset;
        }
        else {
            copyBytes(s->bytes + at, bytes + offset, valid);
            at += valid;
            offset += valid;
        }
    }
    return s;
}

void* bobStringToUtf8(void* s)
{
    uint64_t count = stringCount(s);
    if (count == 0) {
        return nullptr;
    }
    void* array = bobArrayNew(&byteElement, count);
    copyBytes(reinterpret_cast<uint8_t*>(array) + bob::abi::arrayDataOffset, stringBytes(s), count);
    static_cast<BobArray*>(array)->count = count;
    return array;
}

void* bobStringFromInteger(int64_t value, int32_t isUnsigned)
{
    uint8_t digits[24];
    int count = 0;
    bool negative = isUnsigned == 0 && value < 0;
    uint64_t magnitude = negative ? 0 - static_cast<uint64_t>(value) : static_cast<uint64_t>(value);
    do {
        digits[count++] = static_cast<uint8_t>('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude != 0);
    uint8_t text[24];
    int length = 0;
    if (negative) {
        text[length++] = '-';
    }
    while (count > 0) {
        text[length++] = digits[--count];
    }
    return bobStringFromBytes(text, static_cast<uint64_t>(length));
}

// Up to 15 significant digits with trailing zeros removed. Not a shortest
// round-trip conversion (that needs Ryu or similar, planned with the libm work).
void* bobStringFromReal(double value)
{
    uint8_t text[48];
    int length = 0;
    auto put = [&](const char* s) {
        while (*s != '\0') {
            text[length++] = static_cast<uint8_t>(*s++);
        }
    };
    if (value != value) {
        put("nan");
        return bobStringFromBytes(text, static_cast<uint64_t>(length));
    }
    if (value < 0 || (value == 0 && 1 / value < 0)) {
        text[length++] = '-';
        value = -value;
    }
    if (value > 1.7976931348623157e308) {
        put("inf");
        return bobStringFromBytes(text, static_cast<uint64_t>(length));
    }
    int exponent = 0;
    if (value != 0) {
        while (value >= 1e15) {
            value /= 10;
            ++exponent;
        }
        while (value < 1e14) {
            value *= 10;
            --exponent;
        }
    }
    // value is now in [1e14, 1e15): 15 significant digits.
    uint64_t mantissa = static_cast<uint64_t>(value + 0.5);
    if (mantissa >= 1000000000000000ull) {
        mantissa /= 10;
        ++exponent;
    }
    uint8_t digits[16];
    for (int i = 14; i >= 0; --i) {
        digits[i] = static_cast<uint8_t>('0' + mantissa % 10);
        mantissa /= 10;
    }
    int significant = 15;
    while (significant > 1 && digits[significant - 1] == '0') {
        --significant;
    }
    int point = value == 0 ? 1 : 15 + exponent; // digits before the decimal point
    if (point > 15 || point < -4) {
        // d.ddde+x
        text[length++] = digits[0];
        text[length++] = '.';
        for (int i = 1; i < (significant > 1 ? significant : 2); ++i) {
            text[length++] = i < significant ? digits[i] : '0';
        }
        text[length++] = 'e';
        int e = point - 1;
        if (e < 0) {
            text[length++] = '-';
            e = -e;
        }
        uint8_t exp[6];
        int n = 0;
        do {
            exp[n++] = static_cast<uint8_t>('0' + e % 10);
            e /= 10;
        } while (e != 0);
        while (n > 0) {
            text[length++] = exp[--n];
        }
    }
    else if (point <= 0) {
        text[length++] = '0';
        text[length++] = '.';
        for (int i = 0; i < -point; ++i) {
            text[length++] = '0';
        }
        for (int i = 0; i < significant; ++i) {
            text[length++] = digits[i];
        }
    }
    else {
        for (int i = 0; i < point; ++i) {
            text[length++] = digits[i];
        }
        text[length++] = '.';
        if (significant <= point) {
            text[length++] = '0';
        }
        for (int i = point; i < significant; ++i) {
            text[length++] = digits[i];
        }
    }
    return bobStringFromBytes(text, static_cast<uint64_t>(length));
}

void* bobStringFromBool(bool value)
{
    return value ? bobStringFromBytes(reinterpret_cast<const uint8_t*>("true"), 4)
                 : bobStringFromBytes(reinterpret_cast<const uint8_t*>("false"), 5);
}

void* bobStringFromChar(uint32_t codepoint)
{
    uint8_t bytes[4];
    return bobStringFromBytes(bytes, bob::unicode::encodeUtf8(codepoint, bytes));
}

}
