#pragma once

#include "diagnostics/diagnostics.h"
#include "lexer/token.h"

#include <string_view>

namespace bob::lexer {

// Converts source text to a lossless TokenStream. Never stops at an error:
// bad input becomes an `invalid` token plus a diagnostic, so tools (syntax
// highlighters, formatters) always get a complete stream.
class Lexer {
public:
    Lexer(std::string_view text, DiagnosticEngine& diagnostics);

    TokenStream tokenize();

private:
    void lexTrivia(TokenStream& stream);
    Token lexToken();
    Token makeToken(TokenKind kind, uint32_t begin);
    Token lexNumber(uint32_t begin);
    Token lexIdentifier(uint32_t begin);
    Token lexQuoted(uint32_t begin, char quote);
    Token lexOperator(uint32_t begin);

    char peek(uint32_t ahead = 0) const;
    bool atEnd() const { return pos_ >= text_.size(); }

    std::string_view text_;
    DiagnosticEngine& diagnostics_;
    uint32_t pos_ = 0;
};

TokenStream tokenize(std::string_view text, DiagnosticEngine& diagnostics);

}
