#pragma once

// The code generator's implementation class, shared by its source files
// (codeGenerator.cpp, codegenValues.cpp, codegenContainers.cpp).

#include "codegen/codeGenerator.h"

#include "abi.h"
#include "objectModel.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include <functional>
#include <map>
#include <unordered_map>
#include <vector>

namespace bob::codegen::detail {

using namespace bob::ast;
using sema::OverflowMode;
using sema::SendKind;
using sema::Type;
using sema::TypeKind;

class Generator {
public:
    Generator(const sema::SemanticModel& model, llvm::LLVMContext& context, std::string dataLayout)
        : model_(model), context_(context), builder_(context), dataLayout_(std::move(dataLayout))
    {
    }

    std::unique_ptr<llvm::Module> run(const Unit* unit);

private:
    llvm::Type* llvmType(const Type* type);
    llvm::StructType* structType(const sema::RecordInfo* record);
    void declareMessage(const sema::MessageInfo* message);
    void emitMessage(const sema::MessageInfo* message);
    void emitEntryPoint();
    void beginFunction(llvm::Function* function, const sema::MessageInfo* message, const std::vector<sema::LocalInfo*>& locals);

    // classes and objects
    llvm::StructType* objectType(const sema::ClassInfo* info);
    llvm::StructType* classInfoType(size_t vtableSize);
    llvm::GlobalVariable* selectorGlobal(const std::string& selector);
    void declareClass(const sema::ClassInfo* info);
    void emitClass(const sema::ClassInfo* info);
    void emitCopy(const sema::MessageInfo* message);
    // Builds an object: on the heap, or in `place` (a stack slot) when given.
    llvm::Value* emitConstruct(const KeywordSend* send, const sema::SendInfo& info, llvm::Value* place = nullptr);
    // Stack objects (design §4.3a): slots are made when a function starts, the
    // object is built at its declaration, and its fields are released when the
    // frame exits or the declaration runs again.
    void allocateStackObjects(const sema::MessageInfo* message, const sema::BlockInfo* block);
    llvm::Value* emitStackObject(const Node* statement, const sema::StackObject& object);
    void initializeObjectHeader(llvm::Value* object, const sema::ClassInfo* info);
    void destroyStackObjects();
    llvm::Value* fieldAddress(llvm::Value* object, const sema::FieldInfo* field);
    llvm::Value* selfValue();
    llvm::FunctionType* methodType(const sema::MessageInfo* message);

    // reference counting
    void retainValue(llvm::Value* value, const Type* type);
    void releaseValue(llvm::Value* value, const Type* type);
    bool producesOwned(const Node* node) const;
    llvm::Value* emitOwnedAs(const Node* expression, const Type* target);
    void storeReference(llvm::Value* address, llvm::Value* owned, const Type* type);
    void addTemporary(llvm::Value* value, const Type* type);
    void flushTemporaries(size_t mark);
    void releaseLocals();
    void emitReturn(llvm::Value* value);

    void ensureLiveBlock();
    void emitStatements(const std::vector<Node*>& statements);
    void emitStatement(const Node* statement);
    void emitGroup(const Node* group);
    void emitIf(const IfStmt* statement);
    void emitFor(const ForStmt* statement);
    void emitCountingLoop(const sema::LocalInfo* local, const Node* lowNode, const Node* highNode, const std::vector<Node*>& body);
    void emitCase(const CaseStmt* statement);
    void store(const sema::LocalInfo* local, llvm::Value* value);
    llvm::Value* addressOf(const sema::LocalInfo* local);
    llvm::AllocaInst* entryAlloca(llvm::Type* type, const std::string& name);

    llvm::StructType* blockValueType();
    llvm::FunctionType* blockFunctionType(const Type* blockType);
    llvm::Value* emitBlockLiteral(const BlockLiteral* literal);
    llvm::Function* emitBlockFunction(const sema::BlockInfo* info);
    llvm::Value* emitInlinedBody(const BlockLiteral* literal, const Type* resultType);
    llvm::Value* emitInvoke(const Node* receiver, const std::vector<Node*>& arguments, const Type* blockType);
    llvm::Value* emitInlinedIf(const KeywordSend* send, const sema::SendInfo& info);
    llvm::Value* emitInlinedIfNil(const KeywordSend* send, const sema::SendInfo& info);
    void emitTimesRepeat(const KeywordSend* send, const Type* countType);

    llvm::Value* emit(const Node* expression);
    llvm::Value* emitAs(const Node* expression, const Type* target);
    llvm::Value* widen(llvm::Value* value, const Type* from, const Type* to);
    llvm::Value* emitAddress(const Node* expression);
    llvm::Value* emitSend(const Node* node, const sema::SendInfo& send);
    llvm::Value* emitLiteral(const Node* node);

    llvm::Value* arithmetic(char op, llvm::Value* a, llvm::Value* b, const Type* type, OverflowMode mode);
    llvm::Value* divide(char op, llvm::Value* a, llvm::Value* b, const Type* type, OverflowMode mode);
    llvm::Value* negate(llvm::Value* value, const Type* type, OverflowMode mode);
    llvm::Value* compare(const std::string& op, llvm::Value* a, llvm::Value* b, const Type* type);
    llvm::Value* convert(llvm::Value* value, const Type* from, const Type* to);
    llvm::Value* shift(bool left, llvm::Value* value, llvm::Value* amount, const Type* type, const Type* amountType, OverflowMode mode);
    llvm::Value* minValue(const Type* type);
    llvm::Value* maxValue(const Type* type);
    void trapIf(llvm::Value* condition, abi::TrapReason reason);

    // ---- phase 5: values (codegenValues.cpp) ----
    llvm::Function* runtimeFunction(const char* name, llvm::Type* result, std::vector<llvm::Type*> params);
    llvm::Constant* defaultValue(const Type* type);
    uint64_t sizeOf(const Type* type);
    llvm::StructType* unionType(const Type* type);
    // Value conversions the analyzer allowed implicitly: T -> T?, member -> union,
    // checked conversions into subranges. Everything else goes through widen().
    llvm::Value* coerce(llvm::Value* value, const Type* from, const Type* to);
    llvm::Value* optionalPresent(llvm::Value* value, const Type* type);
    llvm::Value* optionalValue(llvm::Value* value, const Type* type);
    llvm::Value* makeOptional(llvm::Value* present, llvm::Value* value, const Type* type);
    llvm::Value* unionTag(llvm::Value* value);
    llvm::Value* unionMember(llvm::Value* value, const Type* unionType, size_t member);
    llvm::Value* unionWrap(llvm::Value* value, const Type* unionType, size_t member);
    size_t unionIndex(const Type* unionType, const Type* member) const;
    // Runs `body(memberIndex, memberValue)` in a switch on the union's tag; returns
    // the phi of the bodies' results when `resultType` isn't void.
    llvm::Value* switchOnUnion(llvm::Value* value, const Type* type, llvm::Type* resultType,
                               const std::function<llvm::Value*(size_t, llvm::Value*)>& body);
    // A function `void (ptr element)` that retains or releases a value of `type` in memory.
    llvm::Function* elementCallback(const Type* type, bool retain);
    llvm::Constant* elementDescriptor(const Type* type);

    llvm::GlobalVariable* enumValues(const sema::EnumInfo* info);
    llvm::Function* enumFindFunction(const sema::EnumInfo* info);
    // Ordinal (i32) of a valid enum value; -1-free.
    llvm::Value* enumOrdinal(llvm::Value* value, const sema::EnumInfo* info);
    llvm::Value* enumMember(llvm::Value* ordinal, const sema::EnumInfo* info);
    llvm::Value* emitEnumSend(const Node* node, const sema::SendInfo& send);
    llvm::Value* emitBitSetSend(const Node* node, const sema::SendInfo& send);
    llvm::Value* bitOf(llvm::Value* member, const Type* setType);
    // Loop over ordinals low..high (i32, inclusive), binding each member to `local`.
    void emitOrdinalLoop(const sema::LocalInfo* local, const Type* elementType, llvm::Value* low, llvm::Value* high,
                         const std::vector<Node*>& body, llvm::Value* setMask);
    llvm::Value* emitOptionalCompare(const BinarySend* send, const sema::SendInfo& info);
    llvm::Value* emitUnionSend(const Node* node, const sema::SendInfo& send);

    // ---- phase 5: strings and containers (codegenContainers.cpp) ----
    llvm::Constant* stringLiteral(const std::string& text);
    llvm::Value* printString(llvm::Value* value, const Type* type);
    llvm::Value* emitStringSend(const Node* node, const sema::SendInfo& send);
    llvm::Value* emitContainerSend(const Node* node, const sema::SendInfo& send);
    llvm::Value* emitArrayLiteral(const ArrayLiteral* literal, const sema::SendInfo& send);
    // The address of a container variable/field for a mutating send; null if none.
    llvm::Value* mutableSlot(const Node* receiver);
    llvm::Value* arrayCount(llvm::Value* array);
    llvm::Value* arrayElementAddress(llvm::Value* array, llvm::Value* index, const Type* element);
    llvm::Value* checkedIndex(llvm::Value* index, const Type* indexType, llvm::Value* count);
    llvm::Value* fixedIndex(llvm::Value* index, const Type* fixedType);
    llvm::Value* spill(llvm::Value* value, const std::string& name);
    // Visits a collection, binding each element (or key and value) and running `body`.
    void emitCollectionLoop(const Node* collection, const Type* type, const std::vector<const sema::LocalInfo*>& locals,
                            const std::vector<Node*>& body, const std::string& selector);

    const sema::SemanticModel& model_;
    llvm::LLVMContext& context_;
    llvm::IRBuilder<> builder_;
    std::string dataLayout_;
    std::unique_ptr<llvm::Module> module_;
    llvm::Function* trapFunction_ = nullptr;
    llvm::Function* retainFunction_ = nullptr;
    llvm::Function* releaseFunction_ = nullptr;
    llvm::Function* allocFunction_ = nullptr;
    llvm::Function* msgSendFunction_ = nullptr;

    struct Temporary {
        llvm::Value* value;
        const Type* type;
    };
    std::vector<Temporary> temporaries_;
    std::unordered_map<const sema::ClassInfo*, llvm::StructType*> objectTypes_;
    std::unordered_map<const sema::ClassInfo*, llvm::GlobalVariable*> classGlobals_;
    std::unordered_map<const sema::ClassInfo*, llvm::Function*> initFunctions_;
    std::unordered_map<const sema::ClassInfo*, llvm::Function*> newFunctions_;
    std::unordered_map<const sema::ClassInfo*, llvm::Function*> initsFunctions_;
    std::unordered_map<const sema::ClassInfo*, llvm::Function*> destroyFunctions_;
    std::unordered_map<std::string, llvm::GlobalVariable*> selectors_;
    llvm::Function* function_ = nullptr;
    const sema::MessageInfo* message_ = nullptr;
    const sema::BlockInfo* block_ = nullptr;
    llvm::Value* environment_ = nullptr;
    std::unordered_map<const sema::MessageInfo*, llvm::Function*> functions_;
    std::unordered_map<const sema::RecordInfo*, llvm::StructType*> structs_;
    std::unordered_map<const sema::LocalInfo*, llvm::AllocaInst*> slots_;
    struct StackSlot {
        llvm::AllocaInst* object;
        llvm::AllocaInst* built;
        const sema::ClassInfo* classInfo;
    };
    std::unordered_map<ast::NodeId, StackSlot> stackSlots_;

    std::unordered_map<const Type*, llvm::StructType*> unionTypes_;
    std::unordered_map<const Type*, llvm::Function*> retainCallbacks_;
    std::unordered_map<const Type*, llvm::Function*> releaseCallbacks_;
    std::unordered_map<const Type*, llvm::Constant*> descriptors_;
    std::unordered_map<const sema::EnumInfo*, llvm::GlobalVariable*> enumValueTables_;
    std::unordered_map<const sema::EnumInfo*, llvm::GlobalVariable*> enumNameTables_;
    std::unordered_map<const sema::EnumInfo*, llvm::Function*> enumFinders_;
    std::map<std::string, llvm::Constant*> stringLiterals_;

    struct LoopTargets {
        llvm::BasicBlock* breakTarget;
        llvm::BasicBlock* continueTarget;
    };
    std::vector<LoopTargets> loops_;
};

}
