#include "lexer/literals.h"

#include <charconv>

namespace bob::lexer {

namespace {

bool isValidScalar(uint32_t c)
{
    return c <= 0x10FFFF && !(c >= 0xD800 && c <= 0xDFFF);
}

int hexValue(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// Decodes one UTF-8 sequence at bytes[i]; returns its length or 0 if invalid.
size_t decodeUtf8At(std::string_view bytes, size_t i, uint32_t& codepoint)
{
    unsigned char b0 = static_cast<unsigned char>(bytes[i]);
    if (b0 < 0x80) {
        codepoint = b0;
        return 1;
    }
    size_t length = 0;
    uint32_t minimum = 0;
    if ((b0 & 0xE0) == 0xC0) {
        length = 2;
        codepoint = b0 & 0x1F;
        minimum = 0x80;
    }
    else if ((b0 & 0xF0) == 0xE0) {
        length = 3;
        codepoint = b0 & 0x0F;
        minimum = 0x800;
    }
    else if ((b0 & 0xF8) == 0xF0) {
        length = 4;
        codepoint = b0 & 0x07;
        minimum = 0x10000;
    }
    else {
        return 0;
    }
    if (i + length > bytes.size()) {
        return 0;
    }
    for (size_t k = 1; k < length; ++k) {
        unsigned char b = static_cast<unsigned char>(bytes[i + k]);
        if ((b & 0xC0) != 0x80) {
            return 0;
        }
        codepoint = (codepoint << 6) | (b & 0x3F);
    }
    if (codepoint < minimum || !isValidScalar(codepoint)) {
        return 0;
    }
    return length;
}

// Decodes the body of a quoted literal (without the quotes) into UTF-8.
std::optional<std::string> decodeQuotedBody(std::string_view body, std::string& out)
{
    size_t i = 0;
    while (i < body.size()) {
        char c = body[i];
        if (c != '\\') {
            uint32_t codepoint = 0;
            size_t length = decodeUtf8At(body, i, codepoint);
            if (length == 0) {
                return std::string("invalid UTF-8 in literal");
            }
            out.append(body.substr(i, length));
            i += length;
            continue;
        }
        if (i + 1 >= body.size()) {
            return std::string("incomplete escape sequence");
        }
        char e = body[i + 1];
        i += 2;
        switch (e) {
        case 'n':
            out += '\n';
            break;
        case 't':
            out += '\t';
            break;
        case 'r':
            out += '\r';
            break;
        case '0':
            out += '\0';
            break;
        case '\\':
            out += '\\';
            break;
        case '"':
            out += '"';
            break;
        case '\'':
            out += '\'';
            break;
        case 'u': {
            if (i >= body.size() || body[i] != '{') {
                return std::string("expected '{' after \\u");
            }
            ++i;
            uint32_t value = 0;
            size_t digits = 0;
            while (i < body.size() && body[i] != '}') {
                int h = hexValue(body[i]);
                if (h < 0 || digits >= 6) {
                    return std::string("invalid \\u{...} escape");
                }
                value = value * 16 + static_cast<uint32_t>(h);
                ++digits;
                ++i;
            }
            if (i >= body.size() || digits == 0) {
                return std::string("invalid \\u{...} escape");
            }
            ++i;
            if (!isValidScalar(value)) {
                return std::string("\\u{...} is not a Unicode scalar value");
            }
            appendUtf8(out, value);
            break;
        }
        default:
            return "unknown escape sequence '\\" + std::string(1, e) + "'";
        }
    }
    return std::nullopt;
}

bool isIntegerSuffix(std::string_view s)
{
    return s == "i8" || s == "i16" || s == "i32" || s == "i64" || s == "i128" || s == "u8" || s == "u16" ||
           s == "u32" || s == "u64" || s == "u128";
}

bool isRealSuffix(std::string_view s)
{
    return s == "r32" || s == "r64";
}

}

void appendUtf8(std::string& out, uint32_t c)
{
    if (c < 0x80) {
        out += static_cast<char>(c);
    }
    else if (c < 0x800) {
        out += static_cast<char>(0xC0 | (c >> 6));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
    else if (c < 0x10000) {
        out += static_cast<char>(0xE0 | (c >> 12));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
    else {
        out += static_cast<char>(0xF0 | (c >> 18));
        out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
}

size_t findInvalidUtf8(std::string_view bytes)
{
    size_t i = 0;
    while (i < bytes.size()) {
        uint32_t codepoint = 0;
        size_t length = decodeUtf8At(bytes, i, codepoint);
        if (length == 0) {
            return i;
        }
        i += length;
    }
    return std::string_view::npos;
}

std::optional<std::string> decodeNumber(std::string_view text, NumberValue& value)
{
    value = NumberValue{};
    std::string digits;
    int base = 10;
    size_t i = 0;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
        i = 2;
    }
    else if (text.size() > 2 && text[0] == '0' && (text[1] == 'b' || text[1] == 'B')) {
        base = 2;
        i = 2;
    }

    bool sawPoint = false;
    bool sawExponent = false;
    for (; i < text.size(); ++i) {
        char c = text[i];
        if (c == '_') {
            continue;
        }
        if (base == 16 && hexValue(c) >= 0) {
            digits += c;
            continue;
        }
        if (base == 2 && (c == '0' || c == '1')) {
            digits += c;
            continue;
        }
        if (base == 10 && c >= '0' && c <= '9') {
            digits += c;
            continue;
        }
        if (base == 10 && c == '.' && !sawPoint && !sawExponent) {
            sawPoint = true;
            digits += c;
            continue;
        }
        if (base == 10 && (c == 'e' || c == 'E') && !sawExponent && i + 1 < text.size() &&
            ((text[i + 1] >= '0' && text[i + 1] <= '9') || text[i + 1] == '+' || text[i + 1] == '-')) {
            sawExponent = true;
            digits += c;
            if (text[i + 1] == '+' || text[i + 1] == '-') {
                digits += text[i + 1];
                ++i;
            }
            continue;
        }
        break;
    }
    value.suffix = std::string(text.substr(i));
    if (digits.empty()) {
        return std::string("number has no digits");
    }
    if (!value.suffix.empty() && !isIntegerSuffix(value.suffix) && !isRealSuffix(value.suffix)) {
        return "unknown literal suffix '" + value.suffix + "'";
    }

    value.isReal = sawPoint || sawExponent || isRealSuffix(value.suffix);
    if (value.isReal) {
        if (base != 10) {
            return std::string("hexadecimal and binary literals can't be real numbers");
        }
        if (isIntegerSuffix(value.suffix)) {
            return "integer suffix '" + value.suffix + "' on a real literal";
        }
        auto result = std::from_chars(digits.data(), digits.data() + digits.size(), value.real);
        if (result.ec != std::errc() || result.ptr != digits.data() + digits.size()) {
            return std::string("invalid real literal");
        }
        return std::nullopt;
    }

    auto result = std::from_chars(digits.data(), digits.data() + digits.size(), value.integer, base);
    if (result.ec == std::errc::result_out_of_range) {
        return std::string("integer literal is too large for 64 bits");
    }
    if (result.ec != std::errc() || result.ptr != digits.data() + digits.size()) {
        return std::string("invalid integer literal");
    }
    return std::nullopt;
}

std::optional<std::string> decodeString(std::string_view text, std::string& value)
{
    value.clear();
    if (text.size() < 2 || text.front() != '"' || text.back() != '"') {
        return std::string("unterminated string literal");
    }
    return decodeQuotedBody(text.substr(1, text.size() - 2), value);
}

std::optional<std::string> decodeChar(std::string_view text, uint32_t& codepoint)
{
    codepoint = 0;
    if (text.size() < 2 || text.front() != '\'' || text.back() != '\'') {
        return std::string("unterminated character literal");
    }
    std::string decoded;
    if (std::optional<std::string> error = decodeQuotedBody(text.substr(1, text.size() - 2), decoded)) {
        return error;
    }
    if (decoded.empty()) {
        return std::string("empty character literal");
    }
    size_t length = decodeUtf8At(decoded, 0, codepoint);
    if (length == 0 || length != decoded.size()) {
        return std::string("character literal must contain exactly one Unicode scalar value");
    }
    return std::nullopt;
}

}
