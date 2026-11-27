#include "parser/parser.h"

#include "lexer/lexer.h"
#include "lexer/literals.h"

#include <algorithm>

namespace bob::parser {

using lexer::Token;
using lexer::TokenKind;
using namespace ast;

namespace {

template <class T>
class ValueRestorer {
public:
    ValueRestorer(T& ref, T value) : ref_(ref), saved_(ref)
    {
        ref_ = value;
    }
    ~ValueRestorer()
    {
        ref_ = saved_;
    }
    ValueRestorer(const ValueRestorer&) = delete;
    ValueRestorer& operator=(const ValueRestorer&) = delete;

private:
    T& ref_;
    T saved_;
};

std::string stripColon(std::string_view keyword)
{
    if (!keyword.empty() && keyword.back() == ':') {
        keyword.remove_suffix(1);
    }
    return std::string(keyword);
}

}

int binaryPrecedence(std::string_view op)
{
    if (op == "or") {
        return 1;
    }
    if (op == "and") {
        return 2;
    }
    if (op == "=" || op == "<>" || op == "<" || op == "<=" || op == ">" || op == ">=") {
        return 3;
    }
    if (op == "*" || op == "/" || op == "%" || op == "&" || op == "shl" || op == "shr") {
        return 5;
    }
    return 4;
}

ParsedUnit parseSource(std::string_view text, DiagnosticEngine& diagnostics)
{
    ParsedUnit result;
    result.tokens = lexer::tokenize(text, diagnostics);
    result.arena = std::make_unique<AstArena>();
    Parser parser(result.tokens, *result.arena, diagnostics);
    result.unit = parser.parseUnit();
    return result;
}

Parser::Parser(const lexer::TokenStream& stream, AstArena& arena, DiagnosticEngine& diagnostics)
    : stream_(stream), arena_(arena), diagnostics_(diagnostics)
{
    for (const Token& token : stream.tokens) {
        if (token.kind != TokenKind::invalid) {
            tokens_.push_back(token);
        }
    }
    if (tokens_.empty() || tokens_.back().kind != TokenKind::endOfFile) {
        Token eof;
        eof.kind = TokenKind::endOfFile;
        tokens_.push_back(eof);
    }
}

// ---------------------------------------------------------------------------
// token helpers

const Token& Parser::peek(size_t ahead) const
{
    size_t index = std::min(pos_ + ahead, tokens_.size() - 1);
    return tokens_[index];
}

bool Parser::at(TokenKind kind, size_t ahead) const
{
    return peek(ahead).kind == kind;
}

bool Parser::atWord(std::string_view word, size_t ahead) const
{
    const Token& token = peek(ahead);
    return token.kind == TokenKind::identifier && token.text == word;
}

bool Parser::atOperator(std::string_view op, size_t ahead) const
{
    const Token& token = peek(ahead);
    return token.kind == TokenKind::binaryOperator && token.text == op;
}

bool Parser::atKeywordPart(size_t ahead) const
{
    return at(TokenKind::keywordSelector, ahead) || (at(TokenKind::identifier, ahead) && at(TokenKind::colon, ahead + 1));
}

const Token& Parser::advance()
{
    const Token& token = tokens_[pos_];
    if (token.kind != TokenKind::endOfFile) {
        ++pos_;
        lastEnd_ = token.range.end;
    }
    return token;
}

bool Parser::accept(TokenKind kind)
{
    if (at(kind)) {
        advance();
        return true;
    }
    return false;
}

const Token& Parser::expect(TokenKind kind, const char* what)
{
    if (!at(kind)) {
        failExpected(what);
    }
    return advance();
}

void Parser::fail(SourceRange range, const char* code, const std::string& message)
{
    diagnostics_.error(range, code, message);
    throw ParseAbort{};
}

void Parser::failExpected(const char* what)
{
    fail(current().range, "parse.expected", std::string("expected ") + what + ", found " + lexer::describeToken(current()));
}

void Parser::expectTerminator()
{
    if (accept(TokenKind::semicolon) || at(TokenKind::rightBrace)) {
        return;
    }
    diagnostics_.error({lastEnd_, lastEnd_}, "parse.expectedSemicolon",
                       "expected ';' after the statement, found " + lexer::describeToken(current()));
}

SourceRange Parser::rangeFrom(uint32_t begin) const
{
    return {begin, std::max(begin, lastEnd_)};
}

std::string Parser::takeKeywordPart(SourceRange* range)
{
    if (at(TokenKind::keywordSelector)) {
        const Token& token = advance();
        if (range != nullptr) {
            *range = {token.range.begin, token.range.end - 1};
        }
        return stripColon(token.text);
    }
    const Token& name = expect(TokenKind::identifier, "a name");
    expect(TokenKind::colon, "':'");
    if (range != nullptr) {
        *range = name.range;
    }
    return std::string(name.text);
}

// ---------------------------------------------------------------------------
// error recovery

void Parser::synchronize()
{
    int depth = 0;
    while (!at(TokenKind::endOfFile)) {
        TokenKind kind = current().kind;
        if (kind == TokenKind::leftBrace || kind == TokenKind::leftParen || kind == TokenKind::leftBracket ||
            kind == TokenKind::attributeOpen) {
            ++depth;
            advance();
            continue;
        }
        if (kind == TokenKind::rightBrace) {
            if (depth == 0) {
                return;
            }
            --depth;
            advance();
            if (depth == 0) {
                accept(TokenKind::semicolon);
                return;
            }
            continue;
        }
        if (kind == TokenKind::rightParen || kind == TokenKind::rightBracket) {
            depth = std::max(0, depth - 1);
            advance();
            continue;
        }
        if (kind == TokenKind::semicolon && depth == 0) {
            advance();
            return;
        }
        advance();
    }
}

Node* Parser::recover(uint32_t begin)
{
    synchronize();
    ErrorNode* node = make<ErrorNode>(begin);
    node->range = rangeFrom(begin);
    return node;
}

template <class ParseOne>
void Parser::parseList(std::vector<Node*>& out, ParseOne parseOne)
{
    while (!at(TokenKind::rightBrace) && !at(TokenKind::endOfFile)) {
        size_t startPos = pos_;
        uint32_t begin = current().range.begin;
        bool savedCaseMode = caseLabelMode_;
        try {
            out.push_back(parseOne());
        }
        catch (const ParseAbort&) {
            pendingPrimary_ = nullptr;
            caseLabelMode_ = savedCaseMode;
            caseLabelEnded_ = false;
            out.push_back(recover(begin));
        }
        if (pos_ == startPos && !at(TokenKind::rightBrace) && !at(TokenKind::endOfFile)) {
            advance();
        }
    }
}

// ---------------------------------------------------------------------------
// attributes, names, unit

std::vector<Attribute*> Parser::parseAttributes()
{
    std::vector<Attribute*> out;
    while (at(TokenKind::attributeOpen)) {
        advance();
        if (!at(TokenKind::rightBracket)) {
            do {
                uint32_t begin = current().range.begin;
                Attribute* attribute = make<Attribute>(begin);
                while (true) {
                    if (at(TokenKind::keywordSelector)) {
                        attribute->name += stripColon(advance().text);
                        attribute->value = parseAttributeValue();
                        break;
                    }
                    attribute->name += expect(TokenKind::identifier, "an attribute name").text;
                    if (at(TokenKind::dot) && (at(TokenKind::identifier, 1) || at(TokenKind::keywordSelector, 1))) {
                        advance();
                        attribute->name += '.';
                        continue;
                    }
                    if (accept(TokenKind::colon)) {
                        attribute->value = parseAttributeValue();
                    }
                    break;
                }
                attribute->range = rangeFrom(begin);
                out.push_back(attribute);
            } while (accept(TokenKind::comma));
        }
        expect(TokenKind::rightBracket, "']' to close the attribute list");
    }
    return out;
}

Node* Parser::parseAttributeValue()
{
    ValueRestorer<bool> caseMode(caseLabelMode_, false);
    return parseBinaryExpression(1);
}

std::string Parser::parseQualifiedName(SourceRange* range)
{
    const Token& first = expect(TokenKind::identifier, "a name");
    std::string name(first.text);
    while (at(TokenKind::dot) && at(TokenKind::identifier, 1)) {
        advance();
        name += '.';
        name += advance().text;
    }
    if (range != nullptr) {
        *range = rangeFrom(first.range.begin);
    }
    return name;
}

std::string Parser::parseVersion()
{
    if (at(TokenKind::versionLiteral) || at(TokenKind::realLiteral) || at(TokenKind::integerLiteral)) {
        return std::string(advance().text);
    }
    failExpected("a version such as 1.0.0");
}

Import* Parser::parseImport()
{
    uint32_t begin = advance().range.begin;
    Import* import = make<Import>(begin);
    import->name = parseQualifiedName(&import->nameRange);
    if (accept(TokenKind::comma)) {
        import->version = parseVersion();
    }
    expectTerminator();
    import->range = rangeFrom(begin);
    return import;
}

Unit* Parser::parseUnit()
{
    Unit* unit = make<Unit>(0);
    try {
        if (at(TokenKind::attributeOpen)) {
            unit->attributes = parseAttributes();
        }
        if ((atWord("program") || atWord("lib") || atWord("module")) && at(TokenKind::identifier, 1)) {
            std::string_view word = advance().text;
            unit->unitKind = word == "program" ? UnitKind::program : word == "lib" ? UnitKind::library : UnitKind::module;
            unit->name = parseQualifiedName(&unit->nameRange);
            if (accept(TokenKind::comma)) {
                unit->version = parseVersion();
            }
            while (atWord("import")) {
                try {
                    unit->imports.push_back(parseImport());
                }
                catch (const ParseAbort&) {
                    synchronize();
                }
            }
        }
        expect(TokenKind::leftBrace, "'{' to open the unit body");
        parseList(unit->items, [this]() { return parseUnitItem(); });
        expect(TokenKind::rightBrace, "'}' to close the unit body");
        if (!at(TokenKind::endOfFile)) {
            diagnostics_.error(current().range, "parse.trailingTokens",
                               "unexpected " + lexer::describeToken(current()) + " after the end of the unit body");
        }
    }
    catch (const ParseAbort&) {
    }
    unit->range = {0, tokens_.back().range.end};
    linkParents(unit);
    return unit;
}

// ---------------------------------------------------------------------------
// declarations

Node* Parser::parseUnitItem()
{
    uint32_t begin = current().range.begin;
    std::vector<Attribute*> attributes;
    if (at(TokenKind::attributeOpen)) {
        attributes = parseAttributes();
    }

    Node* item = nullptr;
    if (atWord("namespace") && at(TokenKind::identifier, 1)) {
        item = parseNamespace();
    }
    else if (atWord("class") && at(TokenKind::identifier, 1)) {
        item = parseClass();
    }
    else if (atWord("record") && at(TokenKind::identifier, 1)) {
        item = parseRecord();
    }
    else if (atWord("enum") && (at(TokenKind::identifier, 1) || at(TokenKind::keywordSelector, 1))) {
        item = parseEnum();
    }
    else if (atWord("type") && (at(TokenKind::keywordSelector, 1) ||
                                (at(TokenKind::identifier, 1) && (at(TokenKind::assign, 2) || at(TokenKind::colon, 2))))) {
        item = parseTypeAlias();
    }
    else if (atWord("extends") && at(TokenKind::identifier, 1)) {
        item = parseExtends();
    }
    else if (atWord("msg") && (at(TokenKind::identifier, 1) || at(TokenKind::keywordSelector, 1) || at(TokenKind::binaryOperator, 1))) {
        item = parseMessage();
        diagnostics_.error(static_cast<MessageDecl*>(item)->selectorRange, "parse.messageOutsideClass",
                           "messages must be declared inside a class or an 'extends' block");
    }
    else {
        return parseStatement(std::move(attributes));
    }
    item->attributes = std::move(attributes);
    item->range.begin = begin;
    return item;
}

Node* Parser::parseNamespace()
{
    uint32_t begin = advance().range.begin;
    NamespaceDecl* decl = make<NamespaceDecl>(begin);
    decl->name = parseQualifiedName(&decl->nameRange);
    expect(TokenKind::leftBrace, "'{' to open the namespace");
    parseList(decl->items, [this]() { return parseUnitItem(); });
    expect(TokenKind::rightBrace, "'}' to close the namespace");
    decl->range = rangeFrom(begin);
    return decl;
}

Node* Parser::parseTypeNameNode()
{
    uint32_t begin = current().range.begin;
    TypeName* type = make<TypeName>(begin);
    type->name = parseQualifiedName(nullptr);
    type->range = rangeFrom(begin);
    return type;
}

void Parser::parseTypeNameList(std::vector<Node*>& out)
{
    do {
        out.push_back(parseTypeNameNode());
    } while (accept(TokenKind::comma));
}

Node* Parser::parseClass()
{
    uint32_t begin = advance().range.begin;
    ClassDecl* decl = make<ClassDecl>(begin);
    const Token& name = expect(TokenKind::identifier, "a class name");
    decl->name = std::string(name.text);
    decl->nameRange = name.range;
    if (atWord("inherits")) {
        advance();
        decl->superclass = parseTypeNameNode();
    }
    if (atWord("implements")) {
        advance();
        parseTypeNameList(decl->protocols);
    }
    expect(TokenKind::leftBrace, "'{' to open the class body");
    parseList(decl->members, [this]() { return parseClassMember(); });
    expect(TokenKind::rightBrace, "'}' to close the class body");
    decl->range = rangeFrom(begin);
    return decl;
}

Node* Parser::parseClassMember()
{
    uint32_t begin = current().range.begin;
    std::vector<Attribute*> attributes;
    if (at(TokenKind::attributeOpen)) {
        attributes = parseAttributes();
    }
    Node* member = nullptr;
    if ((atWord("public") || atWord("private")) && at(TokenKind::leftBrace, 1)) {
        FieldGroup* group = make<FieldGroup>(begin);
        group->visibility = atWord("public") ? Visibility::publicScope : Visibility::privateScope;
        advance();
        advance();
        parseList(group->fields, [this]() { return parseField(); });
        expect(TokenKind::rightBrace, "'}' to close the field group");
        group->range = rangeFrom(begin);
        member = group;
    }
    else if (atWord("msg")) {
        member = parseMessage();
    }
    else {
        failExpected("'public {', 'private {' or 'msg' in the class body");
    }
    member->attributes = std::move(attributes);
    member->range.begin = begin;
    return member;
}

Node* Parser::parseField()
{
    uint32_t begin = current().range.begin;
    std::vector<Attribute*> attributes;
    if (at(TokenKind::attributeOpen)) {
        attributes = parseAttributes();
    }
    if (!atKeywordPart()) {
        failExpected("a field declaration 'name: Type;'");
    }
    FieldDecl* field = make<FieldDecl>(begin);
    field->attributes = std::move(attributes);
    field->name = takeKeywordPart(&field->nameRange);
    field->type = parseType(true);
    if (accept(TokenKind::assign)) {
        field->initializer = parseExpression();
    }
    expectTerminator();
    field->range = rangeFrom(begin);
    return field;
}

Node* Parser::parseRecord()
{
    uint32_t begin = advance().range.begin;
    RecordDecl* decl = make<RecordDecl>(begin);
    const Token& name = expect(TokenKind::identifier, "a record name");
    decl->name = std::string(name.text);
    decl->nameRange = name.range;
    if (atWord("inherits")) {
        advance();
        decl->base = parseTypeNameNode();
    }
    expect(TokenKind::leftBrace, "'{' to open the record body");
    parseList(decl->fields, [this]() { return parseField(); });
    expect(TokenKind::rightBrace, "'}' to close the record body");
    decl->range = rangeFrom(begin);
    return decl;
}

Node* Parser::parseExtends()
{
    uint32_t begin = advance().range.begin;
    ExtendsDecl* decl = make<ExtendsDecl>(begin);
    decl->target = parseTypeNameNode();
    if (atWord("implements")) {
        advance();
        parseTypeNameList(decl->protocols);
    }
    expect(TokenKind::leftBrace, "'{' to open the extends body");
    parseList(decl->members, [this]() {
        uint32_t memberBegin = current().range.begin;
        std::vector<Attribute*> attributes;
        if (at(TokenKind::attributeOpen)) {
            attributes = parseAttributes();
        }
        if (!atWord("msg")) {
            failExpected("'msg' in the extends body");
        }
        Node* member = parseMessage();
        member->attributes = std::move(attributes);
        member->range.begin = memberBegin;
        return member;
    });
    expect(TokenKind::rightBrace, "'}' to close the extends body");
    decl->range = rangeFrom(begin);
    return decl;
}

void Parser::parseDeclNameAndStorage(std::string& name, SourceRange& nameRange, Node*& storage, const char* what)
{
    if (at(TokenKind::keywordSelector)) {
        name = takeKeywordPart(&nameRange);
        storage = parseType(false);
        return;
    }
    const Token& token = expect(TokenKind::identifier, what);
    name = std::string(token.text);
    nameRange = token.range;
    if (accept(TokenKind::colon)) {
        storage = parseType(false);
    }
}

Node* Parser::parseEnum()
{
    uint32_t begin = advance().range.begin;
    EnumDecl* decl = make<EnumDecl>(begin);
    parseDeclNameAndStorage(decl->name, decl->nameRange, decl->storageType, "an enum name");
    expect(TokenKind::leftBrace, "'{' to open the enum body");
    parseList(decl->members, [this]() { return parseEnumMember(); });
    expect(TokenKind::rightBrace, "'}' to close the enum body");
    decl->range = rangeFrom(begin);
    return decl;
}

Node* Parser::parseEnumMember()
{
    const Token& name = expect(TokenKind::identifier, "an enum member name");
    EnumMember* member = make<EnumMember>(name.range.begin);
    member->name = std::string(name.text);
    member->nameRange = name.range;
    if (accept(TokenKind::assign)) {
        member->value = parseExpression();
    }
    expectTerminator();
    member->range = rangeFrom(name.range.begin);
    return member;
}

Node* Parser::parseTypeAlias()
{
    uint32_t begin = advance().range.begin;
    TypeAliasDecl* decl = make<TypeAliasDecl>(begin);
    parseDeclNameAndStorage(decl->name, decl->nameRange, decl->storageType, "a type name");
    expect(TokenKind::assign, "':=' in the type declaration");
    decl->type = parseType(true);
    expectTerminator();
    decl->range = rangeFrom(begin);
    return decl;
}

Node* Parser::parseMessage()
{
    uint32_t begin = advance().range.begin;
    MessageDecl* decl = make<MessageDecl>(begin);
    std::vector<std::string> names;
    auto addParam = [&](std::string label, SourceRange labelRange, std::string name, SourceRange nameRange, uint32_t paramBegin) {
        if (std::find(names.begin(), names.end(), name) != names.end()) {
            diagnostics_.error(nameRange, "parse.duplicateParameter", "duplicate parameter name '" + name + "'");
        }
        names.push_back(name);
        MessageParam* param = make<MessageParam>(paramBegin);
        param->label = std::move(label);
        param->labelRange = labelRange;
        param->name = std::move(name);
        param->nameRange = nameRange;
        param->type = parseType(false);
        param->range = rangeFrom(paramBegin);
        decl->params.push_back(param);
    };

    if (at(TokenKind::binaryOperator)) {
        const Token& op = advance();
        decl->messageKind = MessageKind::binary;
        decl->selector = std::string(op.text);
        decl->selectorRange = op.range;
        if (!atKeywordPart()) {
            failExpected("a parameter 'name:Type' after the binary selector");
        }
        uint32_t paramBegin = current().range.begin;
        SourceRange nameRange;
        std::string name = takeKeywordPart(&nameRange);
        addParam("", {}, name, nameRange, paramBegin);
    }
    else if (atKeywordPart()) {
        decl->messageKind = MessageKind::keyword;
        uint32_t selectorBegin = current().range.begin;
        bool first = true;
        while (atKeywordPart()) {
            uint32_t paramBegin = current().range.begin;
            SourceRange labelRange;
            std::string label = takeKeywordPart(&labelRange);
            SourceRange nameRange = labelRange;
            std::string name;
            if (first) {
                if (!atKeywordPart()) {
                    failExpected("a parameter 'name:Type'");
                }
                name = takeKeywordPart(&nameRange);
            }
            else if (atKeywordPart()) {
                name = takeKeywordPart(&nameRange);
            }
            else {
                name = label;
            }
            decl->selector += label + ":";
            addParam(label, labelRange, name, nameRange, paramBegin);
            first = false;
        }
        decl->selectorRange = rangeFrom(selectorBegin);
    }
    else if (at(TokenKind::identifier)) {
        const Token& name = advance();
        decl->messageKind = MessageKind::unary;
        decl->selector = std::string(name.text);
        decl->selectorRange = name.range;
    }
    else {
        failExpected("a message pattern (unary, binary or keyword)");
    }

    if (at(TokenKind::leftBracket)) {
        decl->returnType = parseBracketType();
    }
    if (!accept(TokenKind::semicolon)) {
        if (!at(TokenKind::leftBrace)) {
            failExpected("'{' to start the message body, or ';' for a required message");
        }
        decl->body = parseStatementGroupBody();
    }
    decl->range = rangeFrom(begin);
    return decl;
}

// ---------------------------------------------------------------------------
// statements

Node* Parser::parseStatement(std::vector<Attribute*> attributes)
{
    uint32_t begin = current().range.begin;
    if (!attributes.empty()) {
        begin = attributes.front()->range.begin;
    }
    if (at(TokenKind::attributeOpen)) {
        std::vector<Attribute*> more = parseAttributes();
        if (attributes.empty() && !more.empty()) {
            begin = more.front()->range.begin;
        }
        attributes.insert(attributes.end(), more.begin(), more.end());
    }

    Node* statement = nullptr;
    if (at(TokenKind::kwReturn)) {
        statement = parseReturn(begin);
    }
    else if (at(TokenKind::kwIf)) {
        statement = parseIf(begin);
    }
    else if (at(TokenKind::kwWhile)) {
        statement = parseWhile(begin);
    }
    else if (at(TokenKind::kwFor)) {
        statement = parseFor(begin);
    }
    else if (at(TokenKind::kwLoop)) {
        statement = parseLoop(begin);
    }
    else if (at(TokenKind::kwBreak) || at(TokenKind::kwContinue)) {
        statement = parseJump(begin);
    }
    else if (at(TokenKind::kwCase)) {
        statement = parseCase(begin);
    }
    else if (atKeywordPart()) {
        statement = parseVariableDecl(begin);
    }
    else if (at(TokenKind::leftBrace)) {
        statement = parseBlockStatement(begin);
    }
    else {
        statement = parseExpressionStatement(begin);
    }
    statement->attributes = std::move(attributes);
    statement->range.begin = begin;
    return statement;
}

Node* Parser::parseReturn(uint32_t begin)
{
    advance();
    ReturnStmt* statement = make<ReturnStmt>(begin);
    if (!at(TokenKind::semicolon) && !at(TokenKind::rightBrace)) {
        do {
            statement->values.push_back(parseExpression());
        } while (accept(TokenKind::comma));
    }
    expectTerminator();
    statement->range = rangeFrom(begin);
    return statement;
}

Node* Parser::parseVariableDecl(uint32_t begin)
{
    VariableDecl* decl = make<VariableDecl>(begin);
    decl->name = takeKeywordPart(&decl->nameRange);
    decl->type = parseType(true);
    if (accept(TokenKind::assign)) {
        decl->initializer = parseExpression();
    }
    expectTerminator();
    decl->range = rangeFrom(begin);
    return decl;
}

bool Parser::newlineBefore(const Token& token) const
{
    for (uint32_t i = 0; i < token.triviaCount; ++i) {
        const lexer::Trivia& trivia = stream_.trivia[token.triviaBegin + i];
        if (trivia.kind == lexer::TriviaKind::newline || trivia.kind == lexer::TriviaKind::blockComment) {
            return true;
        }
    }
    return false;
}

// A `{ ... }` at the start of a statement is a block receiver only when the
// message continues on the same line as its closing brace; otherwise it is a
// statement group and the next line starts a new statement.
bool Parser::continuesExpression() const
{
    if (newlineBefore(current())) {
        return false;
    }
    return at(TokenKind::identifier) || at(TokenKind::keywordSelector) || at(TokenKind::binaryOperator) ||
           at(TokenKind::kwAnd) || at(TokenKind::kwOr) || at(TokenKind::kwShl) || at(TokenKind::kwShr) || at(TokenKind::dot);
}

Node* Parser::parseBlockStatement(uint32_t begin)
{
    Node* block = parseBlockLiteral();
    if (continuesExpression()) {
        pendingPrimary_ = block;
        return parseExpressionStatement(begin);
    }
    auto literal = static_cast<BlockLiteral*>(block);
    if (!literal->params.empty()) {
        diagnostics_.error(literal->range, "parse.groupWithParameters",
                           "a statement group can't have block parameters; send it a message or remove the parameters");
    }
    StatementGroup* group = make<StatementGroup>(begin);
    group->statements = literal->statements;
    accept(TokenKind::semicolon);
    group->range = rangeFrom(begin);
    return group;
}

Node* Parser::parseExpressionStatement(uint32_t begin)
{
    Node* expression = parseExpression();
    if (at(TokenKind::assign)) {
        if (expression->kind != NodeKind::identifier && expression->kind != NodeKind::fieldAccess) {
            fail(expression->range, "parse.invalidAssignmentTarget", "only a variable or a field can be assigned to");
        }
        advance();
        Assignment* assignment = make<Assignment>(begin);
        assignment->target = expression;
        assignment->value = parseExpression();
        expectTerminator();
        assignment->range = rangeFrom(begin);
        return assignment;
    }
    ExpressionStmt* statement = make<ExpressionStmt>(begin);
    statement->expression = expression;
    expectTerminator();
    statement->range = rangeFrom(begin);
    return statement;
}

Node* Parser::parseStatementGroupBody()
{
    const Token& open = expect(TokenKind::leftBrace, "'{'");
    ValueRestorer<bool> condition(conditionMode_, false);
    ValueRestorer<bool> caseSubject(caseSubjectMode_, false);
    StatementGroup* group = make<StatementGroup>(open.range.begin);
    parseList(group->statements, [this]() { return parseStatement({}); });
    expect(TokenKind::rightBrace, "'}' to close the body");
    group->range = rangeFrom(open.range.begin);
    return group;
}

// Conditions are ordinary expressions, but a `{` in operand position would be
// ambiguous with the statement body, so it is rejected unless parenthesised.
Node* Parser::parseCondition()
{
    ValueRestorer<bool> condition(conditionMode_, true);
    return parseExpression();
}

Node* Parser::parseControlBody(const char* statement)
{
    if (!at(TokenKind::leftBrace)) {
        fail(current().range, "parse.expectedBraces",
             std::string("expected '{' to start the body of '") + statement + "'; bodies always need braces, even for one statement");
    }
    return parseStatementGroupBody();
}

Node* Parser::parseIf(uint32_t begin)
{
    advance();
    IfStmt* statement = make<IfStmt>(begin);
    statement->condition = parseCondition();
    statement->thenBody = parseControlBody("if");
    if (accept(TokenKind::kwElse)) {
        if (at(TokenKind::kwIf)) {
            statement->elseBody = parseIf(current().range.begin);
        }
        else {
            statement->elseBody = parseControlBody("else");
        }
    }
    accept(TokenKind::semicolon);
    statement->range = rangeFrom(begin);
    return statement;
}

Node* Parser::parseWhile(uint32_t begin)
{
    advance();
    WhileStmt* statement = make<WhileStmt>(begin);
    statement->condition = parseCondition();
    statement->body = parseControlBody("while");
    accept(TokenKind::semicolon);
    statement->range = rangeFrom(begin);
    return statement;
}

Node* Parser::parseFor(uint32_t begin)
{
    advance();
    ForStmt* statement = make<ForStmt>(begin);
    if (at(TokenKind::keywordSelector)) {
        statement->variable = takeKeywordPart(&statement->variableRange);
        statement->variableType = parseType(false);
    }
    else {
        const Token& name = expect(TokenKind::identifier, "a loop variable name");
        statement->variable = std::string(name.text);
        statement->variableRange = name.range;
        if (accept(TokenKind::colon)) {
            statement->variableType = parseType(false);
        }
    }
    expect(TokenKind::kwIn, "'in' after the loop variable");
    statement->iterable = parseCondition();
    statement->body = parseControlBody("for");
    accept(TokenKind::semicolon);
    statement->range = rangeFrom(begin);
    return statement;
}

Node* Parser::parseLoop(uint32_t begin)
{
    advance();
    LoopStmt* statement = make<LoopStmt>(begin);
    statement->body = parseControlBody("loop");
    accept(TokenKind::semicolon);
    statement->range = rangeFrom(begin);
    return statement;
}

Node* Parser::parseJump(uint32_t begin)
{
    bool isBreak = advance().kind == TokenKind::kwBreak;
    Node* statement = isBreak ? static_cast<Node*>(make<BreakStmt>(begin)) : static_cast<Node*>(make<ContinueStmt>(begin));
    expectTerminator();
    statement->range = rangeFrom(begin);
    return statement;
}

Node* Parser::parseCase(uint32_t begin)
{
    advance();
    CaseStmt* statement = make<CaseStmt>(begin);
    {
        ValueRestorer<bool> caseSubject(caseSubjectMode_, true);
        statement->subject = parseCondition();
    }
    if (!atWord("of")) {
        failExpected("'of' after the case subject");
    }
    advance();
    expect(TokenKind::leftBrace, "'{' to open the case entries");
    parseList(statement->entries, [this]() { return parseCaseEntry(); });
    expect(TokenKind::rightBrace, "'}' to close the case entries");
    if (accept(TokenKind::kwElse)) {
        statement->elseBody = parseControlBody("else");
    }
    accept(TokenKind::semicolon);
    statement->range = rangeFrom(begin);
    return statement;
}

Node* Parser::parseCaseEntry()
{
    uint32_t begin = current().range.begin;
    CaseEntry* entry = make<CaseEntry>(begin);
    {
        ValueRestorer<bool> caseMode(caseLabelMode_, true);
        ValueRestorer<bool> caseEnded(caseLabelEnded_, false);
        while (true) {
            caseLabelEnded_ = false;
            entry->labels.push_back(parseBinaryExpression(1));
            if (caseLabelEnded_) {
                break;
            }
            if (accept(TokenKind::comma)) {
                continue;
            }
            expect(TokenKind::colon, "':' after the case labels");
            break;
        }
    }
    if (!at(TokenKind::leftBrace)) {
        fail(current().range, "parse.expectedBraces", "expected '{' after the case labels; case bodies always need braces");
    }
    entry->body = parseBlockLiteral();
    accept(TokenKind::semicolon);
    entry->range = rangeFrom(begin);
    return entry;
}

// ---------------------------------------------------------------------------
// expressions

Node* Parser::parseExpression()
{
    Node* receiver = parseBinaryExpression(1);
    if (caseLabelMode_ || !at(TokenKind::keywordSelector)) {
        return receiver;
    }
    KeywordSend* send = make<KeywordSend>(receiver->range.begin);
    send->receiver = receiver;
    while (at(TokenKind::keywordSelector)) {
        const Token& part = advance();
        send->selector += part.text;
        send->partRanges.push_back(part.range);
        send->arguments.push_back(parseBinaryExpression(1));
    }
    send->range = rangeFrom(receiver->range.begin);
    return send;
}

Node* Parser::parseBinaryExpression(int minPrecedence)
{
    Node* left = parseUnaryExpression();
    while (!caseLabelEnded_) {
        std::string_view op;
        if (at(TokenKind::binaryOperator)) {
            op = current().text;
        }
        else if (at(TokenKind::kwAnd)) {
            op = "and";
        }
        else if (at(TokenKind::kwOr)) {
            op = "or";
        }
        else if (at(TokenKind::kwShl)) {
            op = "shl";
        }
        else if (at(TokenKind::kwShr)) {
            op = "shr";
        }
        else {
            break;
        }
        int precedence = binaryPrecedence(op);
        if (precedence < minPrecedence) {
            break;
        }
        if (op == "?") {
            fail(current().range, "parse.unexpectedOperator", "'?' is only valid after a type name, as in 'Foo?'");
        }
        const Token& opToken = advance();
        Node* right = parseBinaryExpression(precedence + 1);
        BinarySend* send = make<BinarySend>(left->range.begin);
        send->receiver = left;
        send->selector = std::string(op);
        send->selectorRange = opToken.range;
        send->argument = right;
        send->range = rangeFrom(left->range.begin);
        left = send;
    }
    return left;
}

Node* Parser::parseUnaryExpression()
{
    Node* receiver = parsePostfixExpression();
    while (!caseLabelEnded_) {
        if (caseSubjectMode_ && atWord("of") && at(TokenKind::leftBrace, 1)) {
            break;
        }
        // `name :=` starts the next statement (a forgotten ';'), never a unary send.
        if (at(TokenKind::identifier) && !at(TokenKind::assign, 1)) {
            const Token& selector = advance();
            UnarySend* send = make<UnarySend>(receiver->range.begin);
            send->receiver = receiver;
            send->selector = std::string(selector.text);
            send->selectorRange = selector.range;
            send->range = rangeFrom(receiver->range.begin);
            receiver = send;
        }
        else if (caseLabelMode_ && at(TokenKind::keywordSelector) && at(TokenKind::leftBrace, 1)) {
            SourceRange selectorRange;
            std::string selector = takeKeywordPart(&selectorRange);
            UnarySend* send = make<UnarySend>(receiver->range.begin);
            send->receiver = receiver;
            send->selector = selector;
            send->selectorRange = selectorRange;
            send->range = {receiver->range.begin, selectorRange.end};
            receiver = send;
            caseLabelEnded_ = true;
        }
        else {
            break;
        }
    }
    return receiver;
}

Node* Parser::parsePostfixExpression()
{
    Node* expression = parsePrimary();
    while (at(TokenKind::dot) && at(TokenKind::identifier, 1) && !caseLabelEnded_) {
        advance();
        const Token& field = advance();
        FieldAccess* access = make<FieldAccess>(expression->range.begin);
        access->receiver = expression;
        access->field = std::string(field.text);
        access->fieldRange = field.range;
        access->range = rangeFrom(expression->range.begin);
        expression = access;
    }
    return expression;
}

Node* Parser::parsePrimary()
{
    if (pendingPrimary_ != nullptr) {
        Node* primary = pendingPrimary_;
        pendingPrimary_ = nullptr;
        return primary;
    }
    const Token& token = current();
    uint32_t begin = token.range.begin;
    switch (token.kind) {
    case TokenKind::integerLiteral:
    case TokenKind::realLiteral:
        return parseNumber(false, begin);
    case TokenKind::binaryOperator:
        if (token.text == "-" && (at(TokenKind::integerLiteral, 1) || at(TokenKind::realLiteral, 1))) {
            advance();
            return parseNumber(true, begin);
        }
        break;
    case TokenKind::stringLiteral: {
        advance();
        StringLiteral* literal = make<StringLiteral>(begin);
        lexer::decodeString(token.text, literal->value);
        literal->range = token.range;
        return literal;
    }
    case TokenKind::charLiteral: {
        advance();
        CharLiteral* literal = make<CharLiteral>(begin);
        lexer::decodeChar(token.text, literal->codepoint);
        literal->range = token.range;
        return literal;
    }
    case TokenKind::kwTrue:
    case TokenKind::kwFalse: {
        advance();
        BoolLiteral* literal = make<BoolLiteral>(begin);
        literal->value = token.kind == TokenKind::kwTrue;
        literal->range = token.range;
        return literal;
    }
    case TokenKind::kwNil: {
        advance();
        Node* literal = make<NilLiteral>(begin);
        literal->range = token.range;
        return literal;
    }
    case TokenKind::kwSelf: {
        advance();
        Node* node = make<SelfExpr>(begin);
        node->range = token.range;
        return node;
    }
    case TokenKind::kwSuper: {
        advance();
        Node* node = make<SuperExpr>(begin);
        node->range = token.range;
        return node;
    }
    case TokenKind::identifier: {
        advance();
        Identifier* identifier = make<Identifier>(begin);
        identifier->name = std::string(token.text);
        identifier->range = token.range;
        return identifier;
    }
    case TokenKind::keywordSelector:
        if (caseLabelMode_ && at(TokenKind::leftBrace, 1)) {
            Identifier* identifier = make<Identifier>(begin);
            SourceRange nameRange;
            identifier->name = takeKeywordPart(&nameRange);
            identifier->range = nameRange;
            caseLabelEnded_ = true;
            return identifier;
        }
        break;
    case TokenKind::leftParen: {
        advance();
        ValueRestorer<bool> caseMode(caseLabelMode_, false);
        ValueRestorer<bool> caseEnded(caseLabelEnded_, false);
        ValueRestorer<bool> condition(conditionMode_, false);
        ValueRestorer<bool> caseSubject(caseSubjectMode_, false);
        Node* inner = parseExpression();
        expect(TokenKind::rightParen, "')'");
        return inner;
    }
    case TokenKind::leftBrace:
        if (conditionMode_) {
            fail(token.range, "parse.blockInCondition",
                 "a block can't appear unparenthesised in an if/while/for/case condition; wrap that part of the condition in '( )'");
        }
        return parseBlockLiteral();
    case TokenKind::leftBracket:
        return parseArrayLiteral();
    case TokenKind::attributeOpen: {
        std::vector<Attribute*> attributes = parseAttributes();
        expect(TokenKind::leftParen, "'(' after expression attributes, as in '@[overflow: wrap] (a + b)'");
        ValueRestorer<bool> caseMode(caseLabelMode_, false);
        ValueRestorer<bool> caseEnded(caseLabelEnded_, false);
        ValueRestorer<bool> condition(conditionMode_, false);
        ValueRestorer<bool> caseSubject(caseSubjectMode_, false);
        AttributedExpr* expression = make<AttributedExpr>(begin);
        expression->attributes = std::move(attributes);
        expression->expression = parseExpression();
        expect(TokenKind::rightParen, "')'");
        expression->range = rangeFrom(begin);
        return expression;
    }
    default:
        break;
    }
    failExpected("an expression");
}

Node* Parser::parseNumber(bool negative, uint32_t begin)
{
    const Token& token = advance();
    lexer::NumberValue value;
    lexer::decodeNumber(token.text, value);
    if (value.isReal) {
        RealLiteral* literal = make<RealLiteral>(begin);
        literal->text = (negative ? "-" : "") + std::string(token.text);
        literal->value = negative ? -value.real : value.real;
        literal->suffix = value.suffix;
        literal->range = rangeFrom(begin);
        return literal;
    }
    IntegerLiteral* literal = make<IntegerLiteral>(begin);
    literal->text = (negative ? "-" : "") + std::string(token.text);
    literal->value = value.integer;
    literal->negative = negative;
    literal->suffix = value.suffix;
    literal->range = rangeFrom(begin);
    return literal;
}

Node* Parser::parseBlockLiteral()
{
    const Token& open = expect(TokenKind::leftBrace, "'{'");
    ValueRestorer<bool> caseMode(caseLabelMode_, false);
    ValueRestorer<bool> caseEnded(caseLabelEnded_, false);
    BlockLiteral* block = make<BlockLiteral>(open.range.begin);
    if (at(TokenKind::colon)) {
        while (at(TokenKind::colon)) {
            uint32_t paramBegin = advance().range.begin;
            BlockParam* param = make<BlockParam>(paramBegin);
            if (at(TokenKind::keywordSelector)) {
                param->name = takeKeywordPart(nullptr);
                param->type = parseType(false);
            }
            else {
                param->name = std::string(expect(TokenKind::identifier, "a block parameter name").text);
            }
            param->range = rangeFrom(paramBegin);
            block->params.push_back(param);
        }
        if (!atOperator("|")) {
            failExpected("'|' after the block parameters");
        }
        advance();
    }
    parseList(block->statements, [this]() { return parseStatement({}); });
    expect(TokenKind::rightBrace, "'}' to close the block");
    block->range = rangeFrom(open.range.begin);
    return block;
}

Node* Parser::parseArrayLiteral()
{
    const Token& open = advance();
    ValueRestorer<bool> caseMode(caseLabelMode_, false);
    ValueRestorer<bool> caseEnded(caseLabelEnded_, false);
    ValueRestorer<bool> condition(conditionMode_, false);
    ValueRestorer<bool> caseSubject(caseSubjectMode_, false);
    ArrayLiteral* array = make<ArrayLiteral>(open.range.begin);
    while (!at(TokenKind::rightBracket) && !at(TokenKind::endOfFile)) {
        array->elements.push_back(parseExpression());
        if (!accept(TokenKind::comma)) {
            break;
        }
    }
    expect(TokenKind::rightBracket, "']' to close the array literal");
    array->range = rangeFrom(open.range.begin);
    return array;
}

// ---------------------------------------------------------------------------
// types

Node* Parser::parseType(bool allowTypeMessage)
{
    uint32_t begin = current().range.begin;
    Node* term = parseTypeTerm();
    if (allowTypeMessage && at(TokenKind::keywordSelector)) {
        TypeMessage* message = make<TypeMessage>(begin);
        message->receiver = term;
        while (at(TokenKind::keywordSelector)) {
            message->selector += advance().text;
            message->arguments.push_back(parseTypeTerm());
        }
        message->range = rangeFrom(begin);
        return message;
    }
    if (!isTypeNode(term)) {
        fail(term->range, "parse.expectedType",
             "expected a type; a constant is only valid as part of a type message such as '0 to: 100'");
    }
    return term;
}

Node* Parser::parseTypeTerm()
{
    uint32_t begin = current().range.begin;
    if (at(TokenKind::leftBracket)) {
        return parseOptionalSuffix(parseBracketType(), begin);
    }
    if (at(TokenKind::integerLiteral) || at(TokenKind::realLiteral) || at(TokenKind::charLiteral) ||
        (atOperator("-") && (at(TokenKind::integerLiteral, 1) || at(TokenKind::realLiteral, 1)))) {
        return parsePrimary();
    }
    if (at(TokenKind::identifier)) {
        std::string name = parseQualifiedName(nullptr);
        if (at(TokenKind::identifier) && !at(TokenKind::colon, 1)) {
            Identifier* identifier = make<Identifier>(begin);
            identifier->name = name;
            identifier->range = rangeFrom(begin);
            Node* expression = identifier;
            while (at(TokenKind::identifier) && !at(TokenKind::colon, 1)) {
                const Token& selector = advance();
                UnarySend* send = make<UnarySend>(begin);
                send->receiver = expression;
                send->selector = std::string(selector.text);
                send->selectorRange = selector.range;
                send->range = rangeFrom(begin);
                expression = send;
            }
            return expression;
        }
        TypeName* type = make<TypeName>(begin);
        type->name = name;
        type->range = rangeFrom(begin);
        return parseOptionalSuffix(type, begin);
    }
    failExpected("a type");
}

Node* Parser::parseBracketType()
{
    const Token& open = expect(TokenKind::leftBracket, "'['");
    std::vector<Node*> elements;
    bool named = false;
    if (at(TokenKind::rightBracket)) {
        failExpected("a type inside '[ ]'");
    }
    do {
        uint32_t begin = current().range.begin;
        TupleTypeElement* element = make<TupleTypeElement>(begin);
        if (atKeywordPart()) {
            element->name = takeKeywordPart(nullptr);
            named = true;
        }
        element->type = parseType(true);
        element->range = rangeFrom(begin);
        elements.push_back(element);
    } while (accept(TokenKind::comma));
    expect(TokenKind::rightBracket, "']'");
    if (elements.size() == 1 && !named) {
        return static_cast<TupleTypeElement*>(elements.front())->type;
    }
    TupleType* tuple = make<TupleType>(open.range.begin);
    tuple->elements = std::move(elements);
    tuple->range = rangeFrom(open.range.begin);
    return tuple;
}

Node* Parser::parseOptionalSuffix(Node* inner, uint32_t begin)
{
    if (!atOperator("?")) {
        return inner;
    }
    advance();
    OptionalType* optional = make<OptionalType>(begin);
    optional->inner = inner;
    optional->range = rangeFrom(begin);
    return optional;
}

}
