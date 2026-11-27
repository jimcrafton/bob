#include "lexer/lexer.h"

#include "lexer/literals.h"

namespace bob::lexer {

namespace {

bool isDigit(char c)
{
    return c >= '0' && c <= '9';
}

bool isIdentStart(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool isIdentChar(char c)
{
    return isIdentStart(c) || isDigit(c);
}

bool isHexDigit(char c)
{
    return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

TokenKind reservedWordKind(std::string_view word)
{
    struct Entry {
        std::string_view word;
        TokenKind kind;
    };
    static constexpr Entry reserved[] = {
        {"return", TokenKind::kwReturn},     {"self", TokenKind::kwSelf},   {"super", TokenKind::kwSuper},
        {"nil", TokenKind::kwNil},           {"true", TokenKind::kwTrue},   {"false", TokenKind::kwFalse},
        {"and", TokenKind::kwAnd},           {"or", TokenKind::kwOr},       {"if", TokenKind::kwIf},
        {"else", TokenKind::kwElse},         {"while", TokenKind::kwWhile}, {"for", TokenKind::kwFor},
        {"in", TokenKind::kwIn},             {"loop", TokenKind::kwLoop},   {"break", TokenKind::kwBreak},
        {"continue", TokenKind::kwContinue}, {"case", TokenKind::kwCase},   {"shl", TokenKind::kwShl},
        {"shr", TokenKind::kwShr},
    };
    for (const Entry& entry : reserved) {
        if (entry.word == word) {
            return entry.kind;
        }
    }
    return TokenKind::identifier;
}

}

Lexer::Lexer(std::string_view text, DiagnosticEngine& diagnostics) : text_(text), diagnostics_(diagnostics)
{
}

char Lexer::peek(uint32_t ahead) const
{
    uint32_t index = pos_ + ahead;
    return index < text_.size() ? text_[index] : '\0';
}

TokenStream Lexer::tokenize()
{
    TokenStream stream;
    uint32_t triviaBegin = 0;
    if (text_.substr(0, 3) == "\xEF\xBB\xBF") {
        stream.trivia.push_back({TriviaKind::whitespace, {0, 3}});
        pos_ = 3;
    }
    while (true) {
        lexTrivia(stream);
        Token token = lexToken();
        token.triviaBegin = triviaBegin;
        token.triviaCount = static_cast<uint32_t>(stream.trivia.size()) - triviaBegin;
        triviaBegin = static_cast<uint32_t>(stream.trivia.size());
        stream.tokens.push_back(token);
        if (token.kind == TokenKind::endOfFile) {
            break;
        }
    }
    return stream;
}

void Lexer::lexTrivia(TokenStream& stream)
{
    while (!atEnd()) {
        uint32_t begin = pos_;
        char c = peek();
        if (c == ' ' || c == '\t') {
            while (peek() == ' ' || peek() == '\t') {
                ++pos_;
            }
            stream.trivia.push_back({TriviaKind::whitespace, {begin, pos_}});
        }
        else if (c == '\r' || c == '\n') {
            if (c == '\r' && peek(1) == '\n') {
                pos_ += 2;
            }
            else {
                ++pos_;
            }
            stream.trivia.push_back({TriviaKind::newline, {begin, pos_}});
        }
        else if (c == '#' && peek(1) == '{') {
            pos_ += 2;
            while (!atEnd() && !(peek() == '}' && peek(1) == '#')) {
                ++pos_;
            }
            if (atEnd()) {
                diagnostics_.error({begin, begin + 2}, "lex.unterminatedComment", "unterminated block comment '#{'");
            }
            else {
                pos_ += 2;
            }
            stream.trivia.push_back({TriviaKind::blockComment, {begin, pos_}});
        }
        else if (c == '#') {
            while (!atEnd() && peek() != '\n' && peek() != '\r') {
                ++pos_;
            }
            stream.trivia.push_back({TriviaKind::lineComment, {begin, pos_}});
        }
        else {
            return;
        }
    }
}

Token Lexer::makeToken(TokenKind kind, uint32_t begin)
{
    Token token;
    token.kind = kind;
    token.range = {begin, pos_};
    token.text = text_.substr(begin, pos_ - begin);
    return token;
}

Token Lexer::lexToken()
{
    uint32_t begin = pos_;
    if (atEnd()) {
        return makeToken(TokenKind::endOfFile, begin);
    }
    char c = peek();
    if (isDigit(c)) {
        return lexNumber(begin);
    }
    if (isIdentStart(c)) {
        return lexIdentifier(begin);
    }
    switch (c) {
    case '"':
        return lexQuoted(begin, '"');
    case '\'':
        return lexQuoted(begin, '\'');
    case ':':
        ++pos_;
        if (peek() == '=') {
            ++pos_;
            return makeToken(TokenKind::assign, begin);
        }
        return makeToken(TokenKind::colon, begin);
    case ';':
        ++pos_;
        return makeToken(TokenKind::semicolon, begin);
    case ',':
        ++pos_;
        return makeToken(TokenKind::comma, begin);
    case '.':
        ++pos_;
        return makeToken(TokenKind::dot, begin);
    case '(':
        ++pos_;
        return makeToken(TokenKind::leftParen, begin);
    case ')':
        ++pos_;
        return makeToken(TokenKind::rightParen, begin);
    case '{':
        ++pos_;
        return makeToken(TokenKind::leftBrace, begin);
    case '}':
        ++pos_;
        return makeToken(TokenKind::rightBrace, begin);
    case '[':
        ++pos_;
        return makeToken(TokenKind::leftBracket, begin);
    case ']':
        ++pos_;
        return makeToken(TokenKind::rightBracket, begin);
    case '@':
        ++pos_;
        if (peek() == '[') {
            ++pos_;
            return makeToken(TokenKind::attributeOpen, begin);
        }
        diagnostics_.error({begin, pos_}, "lex.unexpectedCharacter", "'@' must be followed by '[' to start an attribute");
        return makeToken(TokenKind::invalid, begin);
    default:
        break;
    }
    if (std::string_view("+-*/%&|^<>=~?").find(c) != std::string_view::npos) {
        return lexOperator(begin);
    }

    uint32_t length = 1;
    unsigned char lead = static_cast<unsigned char>(c);
    if (lead >= 0x80) {
        uint32_t sequence = (lead & 0xE0) == 0xC0 ? 2 : (lead & 0xF0) == 0xE0 ? 3 : (lead & 0xF8) == 0xF0 ? 4 : 1;
        if (pos_ + sequence <= text_.size() && findInvalidUtf8(text_.substr(pos_, sequence)) == std::string_view::npos) {
            length = sequence;
        }
    }
    pos_ += length;
    diagnostics_.error({begin, pos_}, "lex.unexpectedCharacter",
                       "unexpected character '" + std::string(text_.substr(begin, pos_ - begin)) + "'");
    return makeToken(TokenKind::invalid, begin);
}

Token Lexer::lexNumber(uint32_t begin)
{
    TokenKind kind = TokenKind::integerLiteral;
    if (peek() == '0' && (peek(1) == 'x' || peek(1) == 'X')) {
        pos_ += 2;
        while (isHexDigit(peek()) || peek() == '_') {
            ++pos_;
        }
    }
    else if (peek() == '0' && (peek(1) == 'b' || peek(1) == 'B')) {
        pos_ += 2;
        while (peek() == '0' || peek() == '1' || peek() == '_') {
            ++pos_;
        }
    }
    else {
        while (isDigit(peek()) || peek() == '_') {
            ++pos_;
        }
        if (peek() == '.' && isDigit(peek(1))) {
            kind = TokenKind::realLiteral;
            ++pos_;
            while (isDigit(peek()) || peek() == '_') {
                ++pos_;
            }
            if (peek() == '.' && isDigit(peek(1))) {
                kind = TokenKind::versionLiteral;
                while (peek() == '.' && isDigit(peek(1))) {
                    ++pos_;
                    while (isDigit(peek())) {
                        ++pos_;
                    }
                }
            }
        }
        if (kind != TokenKind::versionLiteral && (peek() == 'e' || peek() == 'E') &&
            (isDigit(peek(1)) || ((peek(1) == '+' || peek(1) == '-') && isDigit(peek(2))))) {
            kind = TokenKind::realLiteral;
            pos_ += 2;
            while (isDigit(peek())) {
                ++pos_;
            }
        }
    }
    if (kind != TokenKind::versionLiteral) {
        while (isIdentChar(peek())) {
            ++pos_;
        }
    }

    Token token = makeToken(kind, begin);
    if (kind != TokenKind::versionLiteral) {
        NumberValue value;
        if (std::optional<std::string> error = decodeNumber(token.text, value)) {
            diagnostics_.error(token.range, "lex.invalidNumber", *error);
        }
        else if (value.isReal) {
            token.kind = TokenKind::realLiteral;
        }
    }
    return token;
}

Token Lexer::lexIdentifier(uint32_t begin)
{
    while (isIdentChar(peek())) {
        ++pos_;
    }
    std::string_view word = text_.substr(begin, pos_ - begin);
    TokenKind reserved = reservedWordKind(word);
    if (reserved != TokenKind::identifier) {
        return makeToken(reserved, begin);
    }
    if (peek() == ':' && peek(1) != '=') {
        ++pos_;
        return makeToken(TokenKind::keywordSelector, begin);
    }
    return makeToken(TokenKind::identifier, begin);
}

Token Lexer::lexQuoted(uint32_t begin, char quote)
{
    ++pos_;
    bool terminated = false;
    while (!atEnd()) {
        char c = peek();
        if (c == '\n' || c == '\r') {
            break;
        }
        if (c == '\\') {
            pos_ += 2;
            continue;
        }
        ++pos_;
        if (c == quote) {
            terminated = true;
            break;
        }
    }
    if (pos_ > text_.size()) {
        pos_ = static_cast<uint32_t>(text_.size());
    }
    TokenKind kind = quote == '"' ? TokenKind::stringLiteral : TokenKind::charLiteral;
    Token token = makeToken(kind, begin);
    if (!terminated) {
        diagnostics_.error(token.range, "lex.unterminatedLiteral",
                           quote == '"' ? "unterminated string literal" : "unterminated character literal");
        token.kind = TokenKind::invalid;
        return token;
    }
    if (kind == TokenKind::stringLiteral) {
        std::string value;
        if (std::optional<std::string> error = decodeString(token.text, value)) {
            diagnostics_.error(token.range, "lex.invalidString", *error);
        }
    }
    else {
        uint32_t codepoint = 0;
        if (std::optional<std::string> error = decodeChar(token.text, codepoint)) {
            diagnostics_.error(token.range, "lex.invalidChar", *error);
        }
    }
    return token;
}

Token Lexer::lexOperator(uint32_t begin)
{
    static constexpr std::string_view twoCharOperators[] = {"<=", ">=", "<>"};
    std::string_view rest = text_.substr(pos_);
    for (std::string_view op : twoCharOperators) {
        if (rest.starts_with(op)) {
            pos_ += 2;
            return makeToken(TokenKind::binaryOperator, begin);
        }
    }
    char c = peek();
    ++pos_;
    if (c == '~') {
        diagnostics_.error({begin, pos_}, "lex.unexpectedCharacter", "unexpected character '~'");
        return makeToken(TokenKind::invalid, begin);
    }
    return makeToken(TokenKind::binaryOperator, begin);
}

TokenStream tokenize(std::string_view text, DiagnosticEngine& diagnostics)
{
    Lexer lexer(text, diagnostics);
    return lexer.tokenize();
}

}
