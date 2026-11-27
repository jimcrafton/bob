#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace bob::lexer {

struct NumberValue {
    bool isReal = false;
    uint64_t integer = 0;
    double real = 0.0;
    std::string suffix;
};

// Each returns an error message on failure. `text` is the full token text
// including quotes for strings/chars.
std::optional<std::string> decodeNumber(std::string_view text, NumberValue& value);
std::optional<std::string> decodeString(std::string_view text, std::string& value);
std::optional<std::string> decodeChar(std::string_view text, uint32_t& codepoint);

// Returns the byte offset of the first invalid UTF-8 sequence, or npos.
size_t findInvalidUtf8(std::string_view bytes);

void appendUtf8(std::string& out, uint32_t codepoint);

}
