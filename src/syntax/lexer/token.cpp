#include "lexer/token.h"

namespace bob::lexer {

const char* tokenKindName(TokenKind kind)
{
    switch (kind) {
    case TokenKind::endOfFile:
        return "endOfFile";
    case TokenKind::invalid:
        return "invalid";
    case TokenKind::identifier:
        return "identifier";
    case TokenKind::keywordSelector:
        return "keywordSelector";
    case TokenKind::integerLiteral:
        return "integerLiteral";
    case TokenKind::realLiteral:
        return "realLiteral";
    case TokenKind::stringLiteral:
        return "stringLiteral";
    case TokenKind::charLiteral:
        return "charLiteral";
    case TokenKind::versionLiteral:
        return "versionLiteral";
    case TokenKind::binaryOperator:
        return "binaryOperator";
    case TokenKind::assign:
        return "assign";
    case TokenKind::colon:
        return "colon";
    case TokenKind::semicolon:
        return "semicolon";
    case TokenKind::comma:
        return "comma";
    case TokenKind::dot:
        return "dot";
    case TokenKind::leftParen:
        return "leftParen";
    case TokenKind::rightParen:
        return "rightParen";
    case TokenKind::leftBrace:
        return "leftBrace";
    case TokenKind::rightBrace:
        return "rightBrace";
    case TokenKind::leftBracket:
        return "leftBracket";
    case TokenKind::rightBracket:
        return "rightBracket";
    case TokenKind::attributeOpen:
        return "attributeOpen";
    case TokenKind::kwReturn:
        return "kwReturn";
    case TokenKind::kwSelf:
        return "kwSelf";
    case TokenKind::kwSuper:
        return "kwSuper";
    case TokenKind::kwNil:
        return "kwNil";
    case TokenKind::kwTrue:
        return "kwTrue";
    case TokenKind::kwFalse:
        return "kwFalse";
    case TokenKind::kwAnd:
        return "kwAnd";
    case TokenKind::kwOr:
        return "kwOr";
    case TokenKind::kwIf:
        return "kwIf";
    case TokenKind::kwElse:
        return "kwElse";
    case TokenKind::kwWhile:
        return "kwWhile";
    case TokenKind::kwFor:
        return "kwFor";
    case TokenKind::kwIn:
        return "kwIn";
    case TokenKind::kwLoop:
        return "kwLoop";
    case TokenKind::kwBreak:
        return "kwBreak";
    case TokenKind::kwContinue:
        return "kwContinue";
    case TokenKind::kwCase:
        return "kwCase";
    }
    return "unknown";
}

const char* triviaKindName(TriviaKind kind)
{
    switch (kind) {
    case TriviaKind::whitespace:
        return "whitespace";
    case TriviaKind::newline:
        return "newline";
    case TriviaKind::lineComment:
        return "lineComment";
    case TriviaKind::blockComment:
        return "blockComment";
    }
    return "unknown";
}

std::string describeToken(const Token& token)
{
    if (token.kind == TokenKind::endOfFile) {
        return "end of file";
    }
    return "'" + std::string(token.text) + "'";
}

bool isReservedWord(TokenKind kind)
{
    switch (kind) {
    case TokenKind::kwReturn:
    case TokenKind::kwSelf:
    case TokenKind::kwSuper:
    case TokenKind::kwNil:
    case TokenKind::kwTrue:
    case TokenKind::kwFalse:
    case TokenKind::kwAnd:
    case TokenKind::kwOr:
    case TokenKind::kwIf:
    case TokenKind::kwElse:
    case TokenKind::kwWhile:
    case TokenKind::kwFor:
    case TokenKind::kwIn:
    case TokenKind::kwLoop:
    case TokenKind::kwBreak:
    case TokenKind::kwContinue:
    case TokenKind::kwCase:
        return true;
    default:
        return false;
    }
}

std::string reconstructSource(const TokenStream& stream, std::string_view source)
{
    std::string out;
    for (const Token& token : stream.tokens) {
        for (uint32_t i = 0; i < token.triviaCount; ++i) {
            const Trivia& trivia = stream.trivia[token.triviaBegin + i];
            out += source.substr(trivia.range.begin, trivia.range.end - trivia.range.begin);
        }
        out += token.text;
    }
    return out;
}

}
