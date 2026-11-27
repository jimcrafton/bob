#pragma once

#include "source/sourceFile.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace bob::ast {

using NodeId = uint32_t;

enum class NodeKind : uint8_t {
    unit,
    import,
    namespaceDecl,
    classDecl,
    recordDecl,
    extendsDecl,
    enumDecl,
    enumMember,
    typeAliasDecl,
    fieldGroup,
    fieldDecl,
    messageDecl,
    messageParam,
    attribute,
    variableDecl,
    assignment,
    returnStmt,
    expressionStmt,
    statementGroup,
    ifStmt,
    whileStmt,
    forStmt,
    loopStmt,
    breakStmt,
    continueStmt,
    caseStmt,
    integerLiteral,
    realLiteral,
    stringLiteral,
    charLiteral,
    boolLiteral,
    nilLiteral,
    selfExpr,
    superExpr,
    identifier,
    fieldAccess,
    arrayLiteral,
    blockLiteral,
    blockParam,
    unarySend,
    binarySend,
    keywordSend,
    caseEntry,
    attributedExpr,
    typeName,
    optionalType,
    tupleType,
    tupleTypeElement,
    typeMessage,
    error,
};

struct Attribute;

// Nodes are owned by an AstArena; links between nodes are non-owning pointers.
// `id` is stable for the arena's lifetime (1-based, 0 = none), suitable for
// tools and scripting bindings to refer to nodes.
struct Node {
    explicit Node(NodeKind nodeKind) : kind(nodeKind) {}
    virtual ~Node() = default;

    NodeKind kind;
    NodeId id = 0;
    SourceRange range;
    Node* parent = nullptr;
    std::vector<Attribute*> attributes;
};

template <NodeKind K>
struct NodeOf : Node {
    static constexpr NodeKind staticKind = K;
    NodeOf() : Node(K) {}
};

enum class UnitKind : uint8_t {
    fragment,
    program,
    library,
    module,
};

enum class Visibility : uint8_t {
    publicScope,
    privateScope,
};

enum class MessageKind : uint8_t {
    unary,
    binary,
    keyword,
};

struct Attribute : NodeOf<NodeKind::attribute> {
    std::string name;
    Node* value = nullptr;
};

struct Import : NodeOf<NodeKind::import> {
    std::string name;
    std::string version;
    SourceRange nameRange;
};

struct Unit : NodeOf<NodeKind::unit> {
    UnitKind unitKind = UnitKind::fragment;
    std::string name;
    std::string version;
    SourceRange nameRange;
    std::vector<Import*> imports;
    std::vector<Node*> items;
};

struct NamespaceDecl : NodeOf<NodeKind::namespaceDecl> {
    std::string name;
    SourceRange nameRange;
    std::vector<Node*> items;
};

struct ClassDecl : NodeOf<NodeKind::classDecl> {
    std::string name;
    SourceRange nameRange;
    Node* superclass = nullptr;
    std::vector<Node*> protocols;
    std::vector<Node*> members;
};

struct RecordDecl : NodeOf<NodeKind::recordDecl> {
    std::string name;
    SourceRange nameRange;
    Node* base = nullptr;
    std::vector<Node*> fields;
};

struct ExtendsDecl : NodeOf<NodeKind::extendsDecl> {
    Node* target = nullptr;
    std::vector<Node*> protocols;
    std::vector<Node*> members;
};

struct EnumMember : NodeOf<NodeKind::enumMember> {
    std::string name;
    SourceRange nameRange;
    Node* value = nullptr;
};

struct EnumDecl : NodeOf<NodeKind::enumDecl> {
    std::string name;
    SourceRange nameRange;
    Node* storageType = nullptr;
    std::vector<Node*> members;
};

struct TypeAliasDecl : NodeOf<NodeKind::typeAliasDecl> {
    std::string name;
    SourceRange nameRange;
    Node* storageType = nullptr;
    Node* type = nullptr;
};

struct FieldDecl : NodeOf<NodeKind::fieldDecl> {
    std::string name;
    SourceRange nameRange;
    Node* type = nullptr;
    Node* initializer = nullptr;
};

struct FieldGroup : NodeOf<NodeKind::fieldGroup> {
    Visibility visibility = Visibility::publicScope;
    std::vector<Node*> fields;
};

struct MessageParam : NodeOf<NodeKind::messageParam> {
    std::string label;
    std::string name;
    SourceRange labelRange;
    SourceRange nameRange;
    Node* type = nullptr;
};

struct StatementGroup;

struct MessageDecl : NodeOf<NodeKind::messageDecl> {
    MessageKind messageKind = MessageKind::unary;
    std::string selector;
    SourceRange selectorRange;
    std::vector<Node*> params;
    Node* returnType = nullptr;
    Node* body = nullptr;
};

struct VariableDecl : NodeOf<NodeKind::variableDecl> {
    std::string name;
    SourceRange nameRange;
    Node* type = nullptr;
    Node* initializer = nullptr;
};

struct Assignment : NodeOf<NodeKind::assignment> {
    Node* target = nullptr;
    Node* value = nullptr;
};

struct ReturnStmt : NodeOf<NodeKind::returnStmt> {
    std::vector<Node*> values;
};

struct ExpressionStmt : NodeOf<NodeKind::expressionStmt> {
    Node* expression = nullptr;
};

struct StatementGroup : NodeOf<NodeKind::statementGroup> {
    std::vector<Node*> statements;
};

// `elseBody` is a StatementGroup, another IfStmt (`else if`), or null.
struct IfStmt : NodeOf<NodeKind::ifStmt> {
    Node* condition = nullptr;
    Node* thenBody = nullptr;
    Node* elseBody = nullptr;
};

struct WhileStmt : NodeOf<NodeKind::whileStmt> {
    Node* condition = nullptr;
    Node* body = nullptr;
};

struct ForStmt : NodeOf<NodeKind::forStmt> {
    std::string variable;
    SourceRange variableRange;
    Node* variableType = nullptr;
    Node* iterable = nullptr;
    Node* body = nullptr;
};

struct LoopStmt : NodeOf<NodeKind::loopStmt> {
    Node* body = nullptr;
};

struct BreakStmt : NodeOf<NodeKind::breakStmt> {
};

struct ContinueStmt : NodeOf<NodeKind::continueStmt> {
};

struct IntegerLiteral : NodeOf<NodeKind::integerLiteral> {
    std::string text;
    uint64_t value = 0;
    bool negative = false;
    std::string suffix;
};

struct RealLiteral : NodeOf<NodeKind::realLiteral> {
    std::string text;
    double value = 0.0;
    std::string suffix;
};

struct StringLiteral : NodeOf<NodeKind::stringLiteral> {
    std::string value;
};

struct CharLiteral : NodeOf<NodeKind::charLiteral> {
    uint32_t codepoint = 0;
};

struct BoolLiteral : NodeOf<NodeKind::boolLiteral> {
    bool value = false;
};

struct NilLiteral : NodeOf<NodeKind::nilLiteral> {
};

struct SelfExpr : NodeOf<NodeKind::selfExpr> {
};

struct SuperExpr : NodeOf<NodeKind::superExpr> {
};

struct Identifier : NodeOf<NodeKind::identifier> {
    std::string name;
};

struct FieldAccess : NodeOf<NodeKind::fieldAccess> {
    Node* receiver = nullptr;
    std::string field;
    SourceRange fieldRange;
};

struct ArrayLiteral : NodeOf<NodeKind::arrayLiteral> {
    std::vector<Node*> elements;
};

struct BlockParam : NodeOf<NodeKind::blockParam> {
    std::string name;
    Node* type = nullptr;
};

struct BlockLiteral : NodeOf<NodeKind::blockLiteral> {
    std::vector<Node*> params;
    std::vector<Node*> statements;
};

struct UnarySend : NodeOf<NodeKind::unarySend> {
    Node* receiver = nullptr;
    std::string selector;
    SourceRange selectorRange;
};

struct BinarySend : NodeOf<NodeKind::binarySend> {
    Node* receiver = nullptr;
    std::string selector;
    SourceRange selectorRange;
    Node* argument = nullptr;
};

struct KeywordSend : NodeOf<NodeKind::keywordSend> {
    Node* receiver = nullptr;
    std::string selector;
    std::vector<SourceRange> partRanges;
    std::vector<Node*> arguments;
};

// The body is a BlockLiteral so a union entry can bind the narrowed value
// (`int32: { :n | ... }`); for enum and integer labels it has no parameters.
struct CaseEntry : NodeOf<NodeKind::caseEntry> {
    std::vector<Node*> labels;
    Node* body = nullptr;
};

struct CaseStmt : NodeOf<NodeKind::caseStmt> {
    Node* subject = nullptr;
    std::vector<Node*> entries;
    Node* elseBody = nullptr;
};

struct AttributedExpr : NodeOf<NodeKind::attributedExpr> {
    Node* expression = nullptr;
};

struct TypeName : NodeOf<NodeKind::typeName> {
    std::string name;
};

struct OptionalType : NodeOf<NodeKind::optionalType> {
    Node* inner = nullptr;
};

struct TupleTypeElement : NodeOf<NodeKind::tupleTypeElement> {
    std::string name;
    Node* type = nullptr;
};

struct TupleType : NodeOf<NodeKind::tupleType> {
    std::vector<Node*> elements;
};

// Compile-time type construction in a type position: `array of: Part`,
// `0 to: 100`, `hashtable keys: K values: V`. Receiver and arguments are type
// nodes or constant expressions.
struct TypeMessage : NodeOf<NodeKind::typeMessage> {
    Node* receiver = nullptr;
    std::string selector;
    std::vector<Node*> arguments;
};

// Placeholder for source the parser couldn't make sense of; spans the skipped text.
struct ErrorNode : NodeOf<NodeKind::error> {
};

class AstArena {
public:
    template <class T>
    T* make(SourceRange range)
    {
        auto node = std::make_unique<T>();
        node->id = static_cast<NodeId>(nodes_.size() + 1);
        node->range = range;
        T* raw = node.get();
        nodes_.push_back(std::move(node));
        return raw;
    }

    Node* byId(NodeId id) const;
    size_t size() const { return nodes_.size(); }

private:
    std::vector<std::unique_ptr<Node>> nodes_;
};

template <class T>
T* nodeCast(Node* node)
{
    return node != nullptr && node->kind == T::staticKind ? static_cast<T*>(node) : nullptr;
}

template <class T>
const T* nodeCast(const Node* node)
{
    return node != nullptr && node->kind == T::staticKind ? static_cast<const T*>(node) : nullptr;
}

// Generic, kind-independent access used by the dumps, tools and scripting.
struct ChildRef {
    const char* role;
    Node* node;
};

struct Property {
    const char* name;
    std::string value;
};

const char* nodeKindName(NodeKind kind);
std::vector<ChildRef> childrenOf(const Node* node);
std::vector<Property> propertiesOf(const Node* node);

void linkParents(Node* root);
Node* findInnermostNode(Node* root, uint32_t offset);
bool isTypeNode(const Node* node);

}
