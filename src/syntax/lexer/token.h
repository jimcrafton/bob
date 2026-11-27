#pragma once

#include "source/sourceFile.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace bob::lexer {

enum class TokenKind : uint8_t {
    endOfFile,
    invalid,
    identifier,
    keywordSelector,
    integerLiteral,
    realLiteral,
    stringLiteral,
    charLiteral,
    versionLiteral,
    binaryOperator,
    assign,
    colon,
    semicolon,
    comma,
    dot,
    leftParen,
    rightParen,
    leftBrace,
    rightBrace,
    leftBracket,
    rightBracket,
    attributeOpen,
    kwReturn,
    kwSelf,
    kwSuper,
    kwNil,
    kwTrue,
    kwFalse,
    kwAnd,
    kwOr,
    kwIf,
    kwElse,
    kwWhile,
    kwFor,
    kwIn,
    kwLoop,
    kwBreak,
    kwContinue,
    kwCase,
    kwShl,
    kwShr,
    kwXor,
};

enum class TriviaKind : uint8_t {
    whitespace,
    newline,
    lineComment,
    blockComment,
};

struct Trivia {
    TriviaKind kind = TriviaKind::whitespace;
    SourceRange range;
};

// Leading trivia (whitespace and comments before the token) is stored in
// TokenStream::trivia[triviaBegin, triviaBegin + triviaCount). Trailing trivia
// of the file belongs to the endOfFile token, so the stream is lossless.
struct Token {
    TokenKind kind = TokenKind::endOfFile;
    SourceRange range;
    std::string_view text;
    uint32_t triviaBegin = 0;
    uint32_t triviaCount = 0;
};

struct TokenStream {
    std::vector<Token> tokens;
    std::vector<Trivia> trivia;
};

const char* tokenKindName(TokenKind kind);
const char* triviaKindName(TriviaKind kind);
std::string describeToken(const Token& token);
bool isReservedWord(TokenKind kind);

std::string reconstructSource(const TokenStream& stream, std::string_view source);

}
