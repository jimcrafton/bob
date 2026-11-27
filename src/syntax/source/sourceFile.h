#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bob {

struct SourceRange {
    uint32_t begin = 0;
    uint32_t end = 0;
};

struct LineColumn {
    uint32_t line = 0;
    uint32_t column = 0;
};

class SourceFile {
public:
    SourceFile(std::string path, std::string text);

    const std::string& path() const { return path_; }
    std::string_view text() const { return text_; }

    LineColumn lineColumn(uint32_t offset) const;
    std::string_view lineText(uint32_t line) const;

private:
    std::string path_;
    std::string text_;
    std::vector<uint32_t> lineStarts_;
};

}
