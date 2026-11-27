// The runtime's UTF-8 and grapheme segmentation (src/runtime/core/unicode.cpp,
// compiled into this test too) against the Unicode conformance data.

#include "unicode.h"

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct BreakCase {
    std::string line;
    std::vector<uint8_t> bytes;
    std::vector<uint64_t> boundaries;
};

// "÷ 0020 × 0308 ÷ 0020 ÷  # comment": ÷ = break, × = no break.
std::vector<BreakCase> readBreakTests()
{
    std::vector<BreakCase> cases;
    std::ifstream in(std::string(BOB_TEST_PROGRAMS_DIR) + "/unicode/GraphemeBreakTest.txt", std::ios::binary);
    std::string line;
    while (std::getline(in, line)) {
        std::string data = line.substr(0, line.find('#'));
        if (data.find_first_not_of(" \t\r") == std::string::npos) {
            continue;
        }
        BreakCase test;
        test.line = line;
        std::istringstream tokens(data);
        std::string token;
        while (tokens >> token) {
            if (token == "\xC3\xB7") {
                if (!test.bytes.empty()) {
                    test.boundaries.push_back(test.bytes.size());
                }
            }
            else if (token != "\xC3\x97") {
                uint8_t encoded[4];
                uint32_t length = bob::unicode::encodeUtf8(static_cast<uint32_t>(std::stoul(token, nullptr, 16)), encoded);
                test.bytes.insert(test.bytes.end(), encoded, encoded + length);
            }
        }
        cases.push_back(test);
    }
    return cases;
}

}

TEST(Unicode, GraphemeBreakConformance)
{
    std::vector<BreakCase> cases = readBreakTests();
    ASSERT_GT(cases.size(), 1000u) << "GraphemeBreakTest.txt not found or empty";
    int failures = 0;
    for (const BreakCase& test : cases) {
        std::vector<uint64_t> actual;
        for (uint64_t offset = 0; offset < test.bytes.size();) {
            offset = bob::unicode::nextGraphemeBoundary(test.bytes.data(), test.bytes.size(), offset);
            actual.push_back(offset);
        }
        if (actual != test.boundaries && ++failures <= 10) {
            ADD_FAILURE() << test.line;
        }
    }
    EXPECT_EQ(failures, 0) << "of " << cases.size() << " cases";
}

TEST(Unicode, Utf8DecodingAndValidation)
{
    const uint8_t text[] = {'a', 0xC3, 0xA9, 0xE2, 0x82, 0xAC, 0xF0, 0x9F, 0x98, 0x80};
    uint64_t offset = 0;
    EXPECT_EQ(bob::unicode::decodeUtf8(text, sizeof(text), offset), 0x61u);
    EXPECT_EQ(bob::unicode::decodeUtf8(text, sizeof(text), offset), 0xE9u);
    EXPECT_EQ(bob::unicode::decodeUtf8(text, sizeof(text), offset), 0x20ACu);
    EXPECT_EQ(bob::unicode::decodeUtf8(text, sizeof(text), offset), 0x1F600u);
    EXPECT_EQ(offset, sizeof(text));
    EXPECT_TRUE(bob::unicode::isValidUtf8(text, sizeof(text)));

    const uint8_t overlong[] = {0xC0, 0xAF};
    const uint8_t surrogate[] = {0xED, 0xA0, 0x80};
    const uint8_t truncated[] = {0xE2, 0x82};
    EXPECT_FALSE(bob::unicode::isValidUtf8(overlong, sizeof(overlong)));
    EXPECT_FALSE(bob::unicode::isValidUtf8(surrogate, sizeof(surrogate)));
    EXPECT_FALSE(bob::unicode::isValidUtf8(truncated, sizeof(truncated)));
}
