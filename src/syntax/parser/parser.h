#pragma once

#include "ast/ast.h"
#include "diagnostics/diagnostics.h"
#include "lexer/token.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace bob::parser {

// The arena owns the tree. Token and AST text refer into the source text, so
// the source must outlive the ParsedUnit.
struct ParsedUnit {
    std::unique_ptr<ast::AstArena> arena;
    lexer::TokenStream tokens;
    ast::Unit* unit = nullptr;
};

ParsedUnit parseSource(std::string_view text, DiagnosticEngine& diagnostics);

// Recursive-descent parser for the grammar in docs/grammar.md. It never fails
// as a whole: syntax errors become diagnostics plus ast::ErrorNode placeholders,
// and parseUnit() always returns a complete tree with parent links set.
class Parser {
public:
    Parser(const lexer::TokenStream& stream, ast::AstArena& arena, DiagnosticEngine& diagnostics);

    ast::Unit* parseUnit();

private:
    struct ParseAbort {};

    const lexer::Token& peek(size_t ahead = 0) const;
    const lexer::Token& current() const { return peek(0); }
    bool at(lexer::TokenKind kind, size_t ahead = 0) const;
    bool atWord(std::string_view word, size_t ahead = 0) const;
    bool atOperator(std::string_view op, size_t ahead = 0) const;
    bool atKeywordPart(size_t ahead = 0) const;
    const lexer::Token& advance();
    bool accept(lexer::TokenKind kind);
    const lexer::Token& expect(lexer::TokenKind kind, const char* what);
    [[noreturn]] void fail(SourceRange range, const char* code, const std::string& message);
    [[noreturn]] void failExpected(const char* what);
    void expectTerminator();
    SourceRange rangeFrom(uint32_t begin) const;
    std::string takeKeywordPart(SourceRange* range);

    template <class T>
    T* make(uint32_t begin)
    {
        return arena_.make<T>(SourceRange{begin, begin});
    }

    void synchronize();
    ast::Node* recover(uint32_t begin);
    template <class ParseOne>
    void parseList(std::vector<ast::Node*>& out, ParseOne parseOne);

    std::vector<ast::Attribute*> parseAttributes();
    ast::Node* parseAttributeValue();
    std::string parseQualifiedName(SourceRange* range);
    std::string parseVersion();
    ast::Import* parseImport();

    ast::Node* parseUnitItem();
    ast::Node* parseNamespace();
    ast::Node* parseClass();
    ast::Node* parseClassMember();
    ast::Node* parseRecord();
    ast::Node* parseExtends();
    ast::Node* parseEnum();
    ast::Node* parseEnumMember();
    ast::Node* parseTypeAlias();
    ast::Node* parseField();
    ast::Node* parseMessage();
    ast::Node* parseTypeNameNode();
    void parseTypeNameList(std::vector<ast::Node*>& out);
    void parseDeclNameAndStorage(std::string& name, SourceRange& nameRange, ast::Node*& storage, const char* what);

    ast::Node* parseStatement(std::vector<ast::Attribute*> attributes);
    ast::Node* parseReturn(uint32_t begin);
    ast::Node* parseVariableDecl(uint32_t begin);
    ast::Node* parseBlockStatement(uint32_t begin);
    ast::Node* parseExpressionStatement(uint32_t begin);
    ast::Node* parseStatementGroupBody();
    ast::Node* parseIf(uint32_t begin);
    ast::Node* parseWhile(uint32_t begin);
    ast::Node* parseFor(uint32_t begin);
    ast::Node* parseLoop(uint32_t begin);
    ast::Node* parseJump(uint32_t begin);
    ast::Node* parseCase(uint32_t begin);
    ast::Node* parseCaseEntry();
    ast::Node* parseCondition();
    ast::Node* parseControlBody(const char* statement);
    bool continuesExpression() const;
    bool newlineBefore(const lexer::Token& token) const;

    ast::Node* parseExpression();
    ast::Node* parseBinaryExpression(int minPrecedence);
    ast::Node* parseUnaryExpression();
    ast::Node* parsePostfixExpression();
    ast::Node* parsePrimary();
    ast::Node* parseNumber(bool negative, uint32_t begin);
    ast::Node* parseBlockLiteral();
    ast::Node* parseArrayLiteral();

    ast::Node* parseType(bool allowTypeMessage);
    ast::Node* parseTypeTerm();
    ast::Node* parseBracketType();
    ast::Node* parseOptionalSuffix(ast::Node* inner, uint32_t begin);

    const lexer::TokenStream& stream_;
    std::vector<lexer::Token> tokens_;
    ast::AstArena& arena_;
    DiagnosticEngine& diagnostics_;
    size_t pos_ = 0;
    uint32_t lastEnd_ = 0;
    ast::Node* pendingPrimary_ = nullptr;
    bool caseLabelMode_ = false;
    bool caseLabelEnded_ = false;
    bool conditionMode_ = false;
    bool caseSubjectMode_ = false;
};

int binaryPrecedence(std::string_view op);

}
