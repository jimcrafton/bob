#pragma once

// The analyzer's implementation class, shared by its source files
// (analyzer.cpp, analyzerTypes.cpp, analyzerContainers.cpp). Not part of the
// library's public interface; tools use analyzer.h and the SemanticModel.

#include "analyzer.h"

#include <initializer_list>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace bob::sema::detail {

using namespace bob::ast;

std::string capitalize(const std::string& name);
bool isArithmeticOperator(std::string_view op);
bool isBitwiseOperator(std::string_view op);
bool isComparisonOperator(std::string_view op);
bool hasAttribute(const Node* node, std::string_view name);
// Number of arguments for `value`, `value:`, `value:value:` ...; -1 otherwise.
int valueSelectorArity(const std::string& selector);
// `ifNil:ifNotNil:` -> {"ifNil:", "ifNotNil:"}.
std::vector<std::string> selectorParts(const std::string& selector);

// Where the statements being checked run: the message body or a block function.
// Inlined blocks keep the context of the code around them.
struct FunctionContext {
    MessageInfo* message = nullptr;
    BlockInfo* block = nullptr;
    std::vector<LocalInfo*>* locals = nullptr;
    bool allowReturn = true;
    int loopDepth = 0;
    // Compile-time code: @[compiletime] messages, expressions and blocks, constants.
    bool compileTime = false;
};

// Escape analysis and stack placement (analyzerEscape.cpp); runs on a complete model.
void analyzeEscapes(SemanticModel& model, DiagnosticEngine& diagnostics);

class Analyzer {
public:
    Analyzer(SemanticModel& model, DiagnosticEngine& diagnostics) : model_(model), types_(model.types), diagnostics_(diagnostics) {}

    void run(const Unit* unit);

private:
    void error(SourceRange range, const char* code, const std::string& message) { diagnostics_.error(range, code, message); }
    void error(const Node* node, const char* code, const std::string& message) { error(node->range, code, message); }
    void unsupported(const Node* node, const std::string& what, const char* when)
    {
        error(node, "sema.unsupported", what + " isn't supported yet (planned for " + when + ")");
    }

    void collect(const Unit* unit);
    void declareRecord(const RecordDecl* decl);
    void resolveRecord(RecordInfo* record);
    void declareProgramMessages(const ExtendsDecl* decl);
    void declareMessage(const MessageDecl* decl);

    void declareClass(const ClassDecl* decl);
    void resolveClass(ClassInfo* info);
    void declareClassMembers(ClassInfo* info, const std::vector<Node*>& members);
    void addProtocols(ClassInfo* info, const std::vector<Node*>& names);
    MessageInfo* declareMethod(ClassInfo* info, const MessageDecl* decl);
    void declareParams(MessageInfo* message, const MessageDecl* decl);
    void completeClasses();
    void completeClass(ClassInfo* info);
    // Construction (analyzerConstruction.cpp).
    MessageInfo* declareInit(ClassInfo* info, const MessageDecl* decl, bool isStatic);
    void addCopyMessage(ClassInfo* info);
    // Whether `x: C;` can build one with every field defaulted; marks required fields.
    bool resolveDefaultable(ClassInfo* info);
    const Type* checkConstructor(const Node* send, ClassInfo* info, const std::string& selector, const std::vector<Node*>& arguments,
                                 SourceRange selectorRange);
    // `x: C;`: checks that C can be default-constructed and records it.
    void checkDefaultObject(const VariableDecl* decl, const Type* type);
    // `@[stack]` / `@[heap]` on a statement: only on declarations that create an object.
    void checkPlacementAttributes(const Node* statement);
    void checkFieldInitializers(ClassInfo* info);
    bool sameSignature(const MessageInfo* a, const MessageInfo* b) const;
    const Type* checkInstanceSend(const Node* send, const Node* receiverNode, const Type* receiver, const std::string& selector,
                                  const std::vector<Node*>& arguments, SourceRange selectorRange);
    const Type* checkClassSideSend(const Node* send, const ClassInfo* info, const std::string& selector,
                                   const std::vector<Node*>& arguments, SourceRange selectorRange);
    const Type* checkReferenceComparison(const BinarySend* send, const Type* left);
    const Type* checkInlinedIfNil(const KeywordSend* send, const Type* receiver, const Type* expected);
    const FieldInfo* selfField(const std::string& name);
    bool canAccessField(const FieldInfo* field) const;
    ClassInfo* currentClass() const { return context_.message != nullptr ? context_.message->owner : nullptr; }
    const Type* resolveType(const Node* node);
    const Type* resolveBlockType(const TypeMessage* message);
    bool requireValueType(const Node* node, const Type* type);
    bool rejectEscapingBlock(const Node* node, const Type* type, const char* where);
    OverflowMode overflowFrom(const Node* node, OverflowMode inherited, std::initializer_list<std::string_view> allowed);

    void ensureChecked(MessageInfo* message);
    void checkMessage(MessageInfo* message);
    void finalizeInferredBlocks(MessageInfo* message);
    void checkStatements(const std::vector<Node*>& statements);
    void checkGroup(const Node* group);
    void checkStatement(const Node* statement);
    void checkAssignment(const Assignment* assignment);
    void checkReturn(const ReturnStmt* statement);
    void checkFor(const ForStmt* statement);
    void checkCase(const CaseStmt* statement);
    void checkCondition(const Node* condition, const char* statement);

    const Type* check(const Node* expression, const Type* expected);
    const Type* checkIntegerLiteral(const IntegerLiteral* literal, const Type* expected);
    const Type* checkRealLiteral(const RealLiteral* literal, const Type* expected);
    const Type* checkIdentifier(const Identifier* identifier);
    const Type* checkFieldAccess(const FieldAccess* access);
    const Type* checkUnary(const UnarySend* send, const Type* expected);
    const Type* checkBinary(const BinarySend* send, const Type* expected);
    const Type* checkKeyword(const KeywordSend* send, const Type* expected);
    const Type* checkOperandPair(const Node* lhs, const Node* rhs, const Type* expected, const char* what, const Node* site);
    void checkArgument(const Node* argument, const Type* parameterType);
    bool requireAssignable(const Node* node, const Type* from, const Type* to, const char* verb);
    bool isUntypedLiteral(const Node* node) const;
    const Type* remember(const Node* node, const Type* type);
    void addSend(const Node* node, SendKind kind, std::string op, const Type* operandType, const Type* resultType);

    const Type* checkBlockLiteral(const BlockLiteral* block, const Type* expected);
    const Type* checkBlockValue(const std::vector<Node*>& statements, const Type* expected, bool hasExpectation);
    const Type* checkBlockInvoke(const Node* send, const Type* blockType, const std::vector<Node*>& arguments, const Type* expected);
    bool unify(const Type* pending, const Type* concrete, const Node* site);
    BlockInfo* newBlockInfo(const BlockLiteral* literal, bool inlined);
    const Type* checkInlinedBranch(const Node* argument, const char* selector, const Type* expected);
    const Type* checkInlinedIf(const KeywordSend* send, const Type* expected);
    const Type* checkInlinedToDo(const KeywordSend* send);
    const Type* checkInlinedTimesRepeat(const KeywordSend* send, const Type* receiver);
    // An inlined block literal with one parameter bound to `elementType` (do:, ifNotNil:, ...).
    bool checkInlinedEach(const Node* argument, const char* selector, const Type* elementType, int loopDepthChange);

    // enums, subranges, named types, optionals of values (analyzerTypes.cpp)
    void declareEnum(const EnumDecl* decl);
    void resolveEnum(EnumInfo* info);
    void declareNamedType(const TypeAliasDecl* decl);
    const Type* resolveNamedType(NamedTypeInfo* info);
    const Type* resolveTypeMessage(const TypeMessage* message, const std::string& name);
    const Type* resolveSubrange(const Node* lowNode, const Node* highNode, const Node* storageNode, const std::string& name, const Node* site);
    // A compile-time integer or enum member in a type position (`0`, `-5`, `Color red`).
    bool typeConstant(const Node* node, const Type*& type, int64_t& value);
    // The value of an already-checked constant expression (integer literal or enum member).
    bool constantBits(const Node* node, uint64_t& bits) const;
    const Type* checkMetaUnary(const UnarySend* send, const Type* meta);
    const Type* checkMetaKeyword(const KeywordSend* send, const Type* meta, const Type* expected);
    const Type* checkValueUnary(const UnarySend* send, const Type* receiver);
    const Type* checkValueKeyword(const KeywordSend* send, const Type* receiver, const Type* expected);
    const Type* checkBitSetBinary(const BinarySend* send, const Type* left);
    const Type* checkOptionalComparison(const BinarySend* send, const Type* left);
    const Type* checkArrayLiteral(const ArrayLiteral* literal, const Type* expected);
    // The element type a `for` loop or `do:` visits for an ordinal iterable, or null.
    const Type* checkOrdinalIterable(const Node* iterable);
    // Checked conversion into a subrange (`pct := n`), validated at run time.
    bool isCheckedConversion(const Type* from, const Type* to) const;
    void checkEnumCase(const CaseStmt* statement, const Type* subject);
    bool isAssignableTarget(const Node* node) const;
    // The type an untyped literal adopts from what is expected (unwrapping T? and unions).
    const Type* literalExpectation(const Type* expected, bool isRealLiteral) const;

    // compile-time code, constants, `meta`, `compiler` (analyzerCompileTime.cpp)
    void declareConstant(const Node* declaration);
    void declareUnitBlock(const StatementGroup* group);
    void checkConstant(ConstantInfo* constant);
    MessageInfo* compileTimeContext(const std::string& name);
    // Checks an item's code in its own context, where runtime variables aren't visible.
    const Type* checkCompileTimeItem(CompileTimeItem* item, const Type* expected);
    const Type* checkCompileTimeExpression(const AttributedExpr* expression, const Type* expected);
    void checkCompileTimeBlock(const StatementGroup* group);
    // A value that can be computed at compile time and embedded in the program.
    bool requireEmbeddable(const Node* node, const Type* type, const char* what);
    const Type* checkMeta(const UnarySend* send, const Type* receiver);
    const Type* checkMetaQuery(const UnarySend* send, const Type* metaType);
    const Type* checkCompilerSend(const KeywordSend* send);
    const ConstValue* makeConst(ConstValue value);
    ClassInfo* fieldMetaClass();
    // Records what the current function calls, for ordering compile-time evaluation.
    void noteCall(const Node* site, const MessageInfo* target);
    void noteConstruct(const ClassInfo* info);

    // strings, containers and unions (analyzerContainers.cpp)
    // Recorded on the `for` statement: the iterable may itself be a send (`s bytes`).
    const Type* checkCollectionIterable(const ForStmt* statement);
    void checkUnionCase(const CaseStmt* statement, const Type* subject);
    const Type* checkStringUnary(const UnarySend* send, const Type* receiver);
    const Type* checkStringKeyword(const KeywordSend* send, const Type* receiver, const Type* expected);
    const Type* checkStringBinary(const BinarySend* send, const Type* left);
    const Type* checkContainerUnary(const UnarySend* send, const Type* receiver);
    const Type* checkContainerKeyword(const KeywordSend* send, const Type* receiver, const Type* expected);
    const Type* checkUnionUnary(const UnarySend* send, const Type* receiver);
    const Type* checkUnionKeyword(const KeywordSend* send, const Type* receiver, const Type* expected);
    // Gives a pending container its element (and key) type; false on a conflict.
    bool settleContainer(const Type* pending, const Type* key, const Type* element, const Node* site);
    // A pending container meets a concrete one (assignment, argument, return).
    bool unifyContainer(const Type* pending, const Type* concrete, const Node* site);
    // The element type a pending container now has, or null while unknown.
    const Type* elementOf(const Type* container, const Node* site, bool required);
    // Reports pending containers whose element type was never found.
    void finalizePendingContainers();
    // A mutating send (`add:`, `at:put:`) needs its receiver to be a variable or field.
    void requireMutable(const Node* receiver, const std::string& selector);
    // The element type a mixed literal stores: one type, a shared protocol, or a union.
    const Type* mergeElementTypes(const std::vector<const Type*>& types, const Node* site);

    void pushScope() { scopes_.emplace_back(); }
    void popScope() { scopes_.pop_back(); }
    LocalInfo* declareLocal(const std::string& name, const Type* type, const Node* decl, SourceRange nameRange);
    LocalInfo* lookupLocal(const std::string& name);
    const LocalInfo* rootLocal(const Node* node) const;
    bool isTypeName(const std::string& name) const;

    bool alwaysReturns(const std::vector<Node*>& statements) const;
    bool alwaysReturns(const Node* statement) const;
    bool breaksOut(const Node* node, int depth) const;

    OverflowMode overflow() const { return overflow_.empty() ? OverflowMode::trap : overflow_.back(); }

    SemanticModel& model_;
    TypeTable& types_;
    DiagnosticEngine& diagnostics_;
    const Unit* unit_ = nullptr;
    FunctionContext context_;
    std::vector<std::unordered_map<std::string, LocalInfo*>> scopes_;
    std::vector<OverflowMode> overflow_;
    OverflowMode unitOverflow_ = OverflowMode::trap;
    std::unordered_map<const MessageInfo*, int> checkState_;
    int blockCounter_ = 0;
};

}
