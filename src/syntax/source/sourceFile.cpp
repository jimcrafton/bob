#include "source/sourceFile.h"

#include <algorithm>

namespace bob {

SourceFile::SourceFile(std::string path, std::string text) : path_(std::move(path)), text_(std::move(text))
{
    lineStarts_.push_back(0);
    for (uint32_t i = 0; i < text_.size(); ++i) {
        if (text_[i] == '\n') {
            lineStarts_.push_back(i + 1);
        }
    }
}

LineColumn SourceFile::lineColumn(uint32_t offset) const
{
    offset = std::min<uint32_t>(offset, static_cast<uint32_t>(text_.size()));
    auto it = std::upper_bound(lineStarts_.begin(), lineStarts_.end(), offset);
    uint32_t lineIndex = static_cast<uint32_t>(it - lineStarts_.begin()) - 1;
    uint32_t column = 1;
    for (uint32_t i = lineStarts_[lineIndex]; i < offset; ++i) {
        if ((static_cast<unsigned char>(text_[i]) & 0xC0) != 0x80) {
            ++column;
        }
    }
    return {lineIndex + 1, column};
}

std::string_view SourceFile::lineText(uint32_t line) const
{
    if (line == 0 || line > lineStarts_.size()) {
        return {};
    }
    uint32_t begin = lineStarts_[line - 1];
    uint32_t end = line < lineStarts_.size() ? lineStarts_[line] : static_cast<uint32_t>(text_.size());
    while (end > begin && (text_[end - 1] == '\n' || text_[end - 1] == '\r')) {
        --end;
    }
    return std::string_view(text_).substr(begin, end - begin);
}

}
