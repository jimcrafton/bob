#pragma once

#include "ast/ast.h"
#include "types.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace bob::sema {

enum class OverflowMode : uint8_t {
    trap,
    wrap,
    saturate,
};

struct ClassInfo;
struct MessageInfo;

struct FieldInfo {
    std::string name;
    const Type* type = nullptr;
    uint32_t index = 0;
    const ast::FieldDecl* decl = nullptr;
    // Class fields only: declaring class, visibility, initializer run by `new`.
    const ClassInfo* owner = nullptr;
    bool isPublic = true;
    const ast::Node* initializer = nullptr;
};

struct RecordInfo {
    std::string name;
    std::vector<FieldInfo> fields;
    bool packed = false;
    const ast::RecordDecl* decl = nullptr;
    Type* type = nullptr;
    bool resolved = false;
    bool resolving = false;

    const FieldInfo* field(std::string_view fieldName) const;
    std::string constructorSelector() const;
};

struct BlockInfo;

struct LocalInfo {
    std::string name;
    const Type* type = nullptr;
    const ast::Node* decl = nullptr;
    bool isParameter = false;
    bool readOnly = false;
    uint32_t index = 0;
    // The block function that owns this variable's storage; null = the message itself.
    const BlockInfo* owner = nullptr;
    // Some block captures it by reference (loads of references are then retained).
    bool captured = false;
};

// A block literal. Inlined blocks (literal arguments of ifTrue:, to:do:, ...)
// run in the enclosing function and own no variables. Other blocks compile to
// their own function and reach captured variables through an environment of
// pointers (capture by reference; blocks don't escape their creating frame).
struct BlockInfo {
    const ast::BlockLiteral* node = nullptr;
    const Type* type = nullptr;
    bool inlined = false;
    const BlockInfo* parent = nullptr;
    std::vector<LocalInfo*> params;
    std::vector<LocalInfo*> locals;
    std::vector<const LocalInfo*> captures;
    std::string symbolName;
};

struct MessageInfo {
    std::string selector;
    std::vector<LocalInfo*> params;
    std::vector<LocalInfo*> locals;
    const Type* returnType = nullptr;
    const ast::MessageDecl* decl = nullptr;
    std::string symbolName;
    OverflowMode overflow = OverflowMode::trap;
    bool isMain = false;

    // Class messages. Instance messages take (self, selector, params...);
    // class-side (@[static]) messages take only their params.
    ClassInfo* owner = nullptr;
    bool isInstance = false;
    bool isProtocolDefault = false;
    bool isRequired = false;
    LocalInfo* self = nullptr;
};

struct ClassInfo {
    std::string name;
    const ast::ClassDecl* decl = nullptr;
    bool isProtocol = false;
    ClassInfo* superclass = nullptr;
    std::vector<ClassInfo*> protocols;
    std::vector<ClassInfo*> subclasses;
    std::vector<FieldInfo> fields;
    // Own instance messages (including protocol defaults adopted here), in order.
    std::vector<MessageInfo*> methodList;
    std::unordered_map<std::string, MessageInfo*> methods;
    std::unordered_map<std::string, MessageInfo*> classMethods;
    // Vtable slots: inherited slots first, then one per new selector.
    std::vector<std::string> slotSelectors;
    std::vector<const MessageInfo*> vtable;
    // Context for field initializers, which run in the generated `new`.
    MessageInfo* initializer = nullptr;
    Type* type = nullptr;
    bool resolved = false;
    bool resolving = false;

    const FieldInfo* field(std::string_view fieldName) const;
    // Class: self or an ancestor or a conformed protocol; protocol: self or a refined protocol.
    bool isKindOf(const ClassInfo* other) const;
    // Instance message visible on this class or protocol (own, inherited, protocol default).
    MessageInfo* lookup(const std::string& selector) const;
    int slotOf(const std::string& selector) const;
};

struct ProgramInfo {
    std::string name;
    std::vector<MessageInfo*> messages;
    std::unordered_map<std::string, MessageInfo*> bySelector;
    MessageInfo* main = nullptr;
    Type* type = nullptr;
};

enum class SendKind : uint8_t {
    arithmetic,
    bitwise,
    shift,
    comparison,
    logicalAnd,
    logicalOr,
    negate,
    absolute,
    boolNot,
    conversion,
    minimum,
    maximum,
    wrapArithmetic,
    saturateArithmetic,
    staticCall,
    recordConstructor,
    interval,
    blockInvoke,
    inlineIf,
    inlineToDo,
    inlineTimesRepeat,
    inlineIfNil,
    newObject,
    classStaticCall,
    directMethodCall,
    vtableCall,
    dynamicSend,
    superCall,
    fieldGet,
    fieldSet,
    referenceCompare,
};

// How a send node is implemented. `operandType` is the type every operand is
// converted to before the operation; `op` is the operator ("+", "<") for
// arithmetic-like kinds.
struct SendInfo {
    SendKind kind = SendKind::staticCall;
    std::string op;
    const Type* operandType = nullptr;
    const Type* resultType = nullptr;
    OverflowMode overflow = OverflowMode::trap;
    const MessageInfo* target = nullptr;
    const RecordInfo* record = nullptr;
    const ClassInfo* classInfo = nullptr;
    const FieldInfo* field = nullptr;
    int slot = -1;
};

// One case label as an inclusive range of the subject's values, stored as
// two's-complement bit patterns and compared with the subject's signedness.
struct CaseRange {
    uint64_t low = 0;
    uint64_t high = 0;
};

// All results are keyed by AST node id so tools can query them for any node.
class SemanticModel {
public:
    TypeTable types;
    ProgramInfo program;
    std::vector<std::unique_ptr<RecordInfo>> records;
    std::vector<std::unique_ptr<MessageInfo>> messages;
    std::vector<std::unique_ptr<LocalInfo>> locals;
    std::vector<std::unique_ptr<BlockInfo>> blockInfos;
    std::vector<std::unique_ptr<ClassInfo>> classes;

    std::unordered_map<ast::NodeId, const Type*> expressionTypes;
    std::unordered_map<ast::NodeId, const BlockInfo*> blocks;
    std::unordered_map<ast::NodeId, const LocalInfo*> localRefs;
    std::unordered_map<ast::NodeId, SendInfo> sends;
    std::unordered_map<ast::NodeId, const FieldInfo*> fieldRefs;
    // Bare identifiers inside instance messages that name a field of self.
    std::unordered_map<ast::NodeId, const FieldInfo*> selfFieldRefs;
    std::unordered_map<ast::NodeId, std::vector<CaseRange>> caseRanges;
    std::unordered_set<ast::NodeId> implicitDeclarations;

    const Type* typeOf(const ast::Node* node) const;
    const LocalInfo* localOf(const ast::Node* node) const;
    const SendInfo* sendOf(const ast::Node* node) const;
    const FieldInfo* fieldOf(const ast::Node* node) const;
    const BlockInfo* blockOf(const ast::Node* node) const;
    const RecordInfo* recordNamed(std::string_view name) const;
    ClassInfo* classNamed(std::string_view name) const;
};

}
