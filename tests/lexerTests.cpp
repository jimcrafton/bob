#include "lexer/lexer.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using bob::DiagnosticEngine;
using bob::lexer::TokenKind;
using bob::lexer::TokenStream;

namespace {

std::vector<TokenKind> kinds(const std::string& text, DiagnosticEngine* diagnosticsOut = nullptr)
{
    DiagnosticEngine diagnostics;
    TokenStream stream = bob::lexer::tokenize(text, diagnostics);
    std::vector<TokenKind> out;
    for (const auto& token : stream.tokens) {
        out.push_back(token.kind);
    }
    if (diagnosticsOut != nullptr) {
        *diagnosticsOut = diagnostics;
    }
    return out;
}

std::vector<std::string> texts(const std::string& text)
{
    DiagnosticEngine diagnostics;
    TokenStream stream = bob::lexer::tokenize(text, diagnostics);
    std::vector<std::string> out;
    for (const auto& token : stream.tokens) {
        if (token.kind != TokenKind::endOfFile) {
            out.emplace_back(token.text);
        }
    }
    return out;
}

}

TEST(Lexer, KeywordSelectorNeedsAdjacentColon)
{
    EXPECT_EQ(kinds("at: put:"), (std::vector<TokenKind>{TokenKind::keywordSelector, TokenKind::keywordSelector, TokenKind::endOfFile}));
    EXPECT_EQ(kinds("at :"), (std::vector<TokenKind>{TokenKind::identifier, TokenKind::colon, TokenKind::endOfFile}));
    EXPECT_EQ(kinds("x:=1"), (std::vector<TokenKind>{TokenKind::identifier, TokenKind::assign, TokenKind::integerLiteral, TokenKind::endOfFile}));
}

TEST(Lexer, ReservedAndContextualWords)
{
    EXPECT_EQ(kinds("self super nil true false return and or"),
              (std::vector<TokenKind>{TokenKind::kwSelf, TokenKind::kwSuper, TokenKind::kwNil, TokenKind::kwTrue, TokenKind::kwFalse,
                                      TokenKind::kwReturn, TokenKind::kwAnd, TokenKind::kwOr, TokenKind::endOfFile}));
    EXPECT_EQ(kinds("class msg type record enum"),
              (std::vector<TokenKind>{TokenKind::identifier, TokenKind::identifier, TokenKind::identifier, TokenKind::identifier,
                                      TokenKind::identifier, TokenKind::endOfFile}));
}

TEST(Lexer, Numbers)
{
    EXPECT_EQ(kinds("123 0x7F 0b1010 1_000_000 10u8"),
              (std::vector<TokenKind>{TokenKind::integerLiteral, TokenKind::integerLiteral, TokenKind::integerLiteral,
                                      TokenKind::integerLiteral, TokenKind::integerLiteral, TokenKind::endOfFile}));
    EXPECT_EQ(kinds("3.25 1e-9 3.0r32 2r64"),
              (std::vector<TokenKind>{TokenKind::realLiteral, TokenKind::realLiteral, TokenKind::realLiteral, TokenKind::realLiteral,
                                      TokenKind::endOfFile}));
    EXPECT_EQ(kinds("1.4.11"), (std::vector<TokenKind>{TokenKind::versionLiteral, TokenKind::endOfFile}));
    EXPECT_EQ(kinds("p.x 1.foo"), (std::vector<TokenKind>{TokenKind::identifier, TokenKind::dot, TokenKind::identifier,
                                                            TokenKind::integerLiteral, TokenKind::dot, TokenKind::identifier,
                                                            TokenKind::endOfFile}));
}

TEST(Lexer, BadNumberSuffixIsReported)
{
    DiagnosticEngine diagnostics;
    kinds("10q7", &diagnostics);
    ASSERT_EQ(diagnostics.errorCount(), 1u);
    EXPECT_EQ(diagnostics.diagnostics()[0].code, "lex.invalidNumber");
}

TEST(Lexer, Operators)
{
    EXPECT_EQ(texts("a<=b>=c=d<e>f|g^h&i%j*-1<>k"),
              (std::vector<std::string>{"a", "<=", "b", ">=", "c", "=", "d", "<", "e", ">", "f", "|", "g", "^", "h", "&", "i", "%",
                                        "j", "*", "-", "1", "<>", "k"}));
    EXPECT_EQ(kinds("x shl 2 shr 1"), (std::vector<TokenKind>{TokenKind::identifier, TokenKind::kwShl, TokenKind::integerLiteral,
                                                               TokenKind::kwShr, TokenKind::integerLiteral, TokenKind::endOfFile}));
}

TEST(Lexer, RemovedOperatorsAreNotSingleTokens)
{
    EXPECT_EQ(texts("a<<b>>c==d??e"), (std::vector<std::string>{"a", "<", "<", "b", ">", ">", "c", "=", "=", "d", "?", "?", "e"}));
    DiagnosticEngine diagnostics;
    kinds("a ~= b", &diagnostics);
    EXPECT_EQ(diagnostics.errorCount(), 1u);
}

TEST(Lexer, AttributeOpenAndPunctuation)
{
    EXPECT_EQ(kinds("@[static] ( ) [ ] { } ; , ."),
              (std::vector<TokenKind>{TokenKind::attributeOpen, TokenKind::identifier, TokenKind::rightBracket, TokenKind::leftParen,
                                      TokenKind::rightParen, TokenKind::leftBracket, TokenKind::rightBracket, TokenKind::leftBrace,
                                      TokenKind::rightBrace, TokenKind::semicolon, TokenKind::comma, TokenKind::dot,
                                      TokenKind::endOfFile}));
}

TEST(Lexer, StringsAndChars)
{
    DiagnosticEngine diagnostics;
    EXPECT_EQ(kinds("\"h\\u{E9}llo\\n\" 'é' '\\t'", &diagnostics),
              (std::vector<TokenKind>{TokenKind::stringLiteral, TokenKind::charLiteral, TokenKind::charLiteral, TokenKind::endOfFile}));
    EXPECT_EQ(diagnostics.errorCount(), 0u);
}

TEST(Lexer, StringErrors)
{
    DiagnosticEngine unterminated;
    EXPECT_EQ(kinds("\"abc\nx", &unterminated).front(), TokenKind::invalid);
    EXPECT_EQ(unterminated.diagnostics().at(0).code, "lex.unterminatedLiteral");

    DiagnosticEngine badEscape;
    kinds("\"\\q\"", &badEscape);
    EXPECT_EQ(badEscape.diagnostics().at(0).code, "lex.invalidString");

    DiagnosticEngine badUtf8;
    kinds("\"\xC3\x28\"", &badUtf8);
    EXPECT_EQ(badUtf8.diagnostics().at(0).code, "lex.invalidString");

    DiagnosticEngine twoChars;
    kinds("'ab'", &twoChars);
    EXPECT_EQ(twoChars.diagnostics().at(0).code, "lex.invalidChar");
}

TEST(Lexer, CommentsAreTrivia)
{
    DiagnosticEngine diagnostics;
    TokenStream stream = bob::lexer::tokenize("a # line\n#{ block\n }# b", diagnostics);
    ASSERT_EQ(stream.tokens.size(), 3u);
    const auto& b = stream.tokens[1];
    EXPECT_EQ(b.text, "b");
    std::vector<bob::lexer::TriviaKind> trivia;
    for (uint32_t i = 0; i < b.triviaCount; ++i) {
        trivia.push_back(stream.trivia[b.triviaBegin + i].kind);
    }
    EXPECT_EQ(trivia, (std::vector<bob::lexer::TriviaKind>{bob::lexer::TriviaKind::whitespace, bob::lexer::TriviaKind::lineComment,
                                                           bob::lexer::TriviaKind::newline, bob::lexer::TriviaKind::blockComment,
                                                           bob::lexer::TriviaKind::whitespace}));
}

TEST(Lexer, UnterminatedBlockComment)
{
    DiagnosticEngine diagnostics;
    kinds("a #{ never closed", &diagnostics);
    EXPECT_EQ(diagnostics.diagnostics().at(0).code, "lex.unterminatedComment");
}

TEST(Lexer, InvalidCharactersDontStopLexing)
{
    DiagnosticEngine diagnostics;
    std::vector<TokenKind> result = kinds("a $ b ~ c \xE2\x82\xAC d", &diagnostics);
    ASSERT_EQ(diagnostics.diagnostics().size(), 3u);
    EXPECT_EQ(diagnostics.diagnostics()[1].message, "unexpected character '~'");
    EXPECT_EQ(result, (std::vector<TokenKind>{TokenKind::identifier, TokenKind::invalid, TokenKind::identifier, TokenKind::invalid,
                                              TokenKind::identifier, TokenKind::invalid, TokenKind::identifier, TokenKind::endOfFile}));
    EXPECT_EQ(diagnostics.errorCount(), 3u);
}

TEST(Lexer, StreamIsLossless)
{
    const std::string sources[] = {
        "",
        "\xEF\xBB\xBF{ x := 1; }\r\n",
        "  # leading comment\nprogram p, 1.0.0 {\n\t#{ block }# y: int32 := -4;   \n}\n# trailing",
        "bad $ chars \"unterminated\n ok",
    };
    for (const std::string& source : sources) {
        DiagnosticEngine diagnostics;
        TokenStream stream = bob::lexer::tokenize(source, diagnostics);
        EXPECT_EQ(bob::lexer::reconstructSource(stream, source), source);
    }
}
