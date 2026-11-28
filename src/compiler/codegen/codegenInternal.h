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

// How one value crosses the C ABI (Windows x64, design §6.6).
struct CPassing {
    enum class Kind : uint8_t {
        direct,    // in its own LLVM type
        asInteger, // a record of 1, 2, 4 or 8 bytes, as an integer of that size
        indirect,  // any other record: a pointer to a copy (for a result: a hidden sret pointer)
    };
    Kind kind = Kind::direct;
    llvm::Type* type = nullptr;
};

struct CSignature {
    llvm::FunctionType* type = nullptr;
    std::vector<CPassing> params;
    CPassing result;
    bool sret = false;
    std::vector<const sema::Type*> paramTypes;
    const sema::Type* resultType = nullptr;
};

class Generator {
public:
    Generator(const sema::SemanticModel& model, llvm::LLVMContext& context, std::string dataLayout)
        : model_(model), context_(context), builder_(context), dataLayout_(std::move(dataLayout))
    {
    }

    std::unique_ptr<llvm::Module> run(const Unit* unit);

    // Evaluation mode: build the module that runs one compile-time item (see
    // generateEvaluationModule) instead of the program.
    void evaluate(const sema::CompileTimeItem* item, std::string thunk, std::string triple)
    {
        evaluating_ = item;
        thunkName_ = std::move(thunk);
        triple_ = std::move(triple);
    }
    uint64_t measure(const Type* type);

private:
    llvm::Type* llvmType(const Type* type);
    llvm::StructType* structType(const sema::RecordInfo* record);
    // The LLVM element of record field `index` (`@[align:]` markers shift them).
    unsigned recordSlot(const sema::RecordInfo* record, size_t index);
    void declareMessage(const sema::MessageInfo* message);
    void emitMessage(const sema::MessageInfo* message);
    void emitEntryPoint();
    void beginFunction(llvm::Function* function, const sema::MessageInfo* message, const std::vector<sema::LocalInfo*>& locals);

    // classes and objects
    llvm::StructType* objectType(const sema::ClassInfo* info);
    llvm::StructType* classInfoType(size_t vtableSize);
    llvm::GlobalVariable* selectorGlobal(const std::string& selector);
    // Protocol dispatch through itables (docs/proposals.md §2).
    llvm::GlobalVariable* protocolGlobal(const sema::ClassInfo* protocol);
    llvm::Function* witnessFunction();
    std::pair<llvm::Constant*, uint32_t> emitConformances(const sema::ClassInfo* info);
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
    llvm::Function* reflectionThunk(const sema::MessageInfo* message);

    // reference counting
    void retainValue(llvm::Value* value, const Type* type);
    void releaseValue(llvm::Value* value, const Type* type);
    bool producesOwned(const Node* node) const;
    llvm::Value* emitOwnedAs(const Node* expression, const Type* target);
    void storeReference(llvm::Value* address, llvm::Value* owned, const Type* type);
    // Locals that only ever hold stack objects get no retain/release at all.
    bool isStackOnly(const sema::LocalInfo* local) const;
    bool isStackOnlyValue(const Node* expression) const;
    void storeLocal(const sema::LocalInfo* local, llvm::Value* owned);
    void addTemporary(llvm::Value* value, const Type* type);
    void flushTemporaries(size_t mark);
    void releaseLocals();
    void emitReturn(llvm::Value* value);

    // LLVM 23's getTerminator() asserts a well-formed block instead of
    // returning null, so this works on both 22 and 23.
    bool blockTerminated() const
    {
        const llvm::BasicBlock* block = builder_.GetInsertBlock();
        return !block->empty() && block->back().isTerminator();
    }
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

    // ---- closures (codegenClosures.cpp) ----
    // A block value is {function, environment}. The environment is null (nothing
    // captured) or an object: a stack record with the immortal flag for blocks
    // that don't outlive their frame, otherwise a reference-counted heap closure.
    llvm::StructType* blockValueType();
    llvm::FunctionType* blockFunctionType(const Type* blockType);
    llvm::Value* emitBlockLiteral(const BlockLiteral* literal);
    bool isHeapBlock(const sema::BlockInfo* info) const;
    // A heap closure holds read-only captures (parameters, self, loop variables)
    // by value; everything else is the address of the variable (inside its box,
    // for a boxed variable).
    bool capturedByValue(const sema::BlockInfo* info, const sema::LocalInfo* local) const;
    llvm::StructType* environmentType(const sema::BlockInfo* info);
    llvm::GlobalVariable* environmentClass(const sema::BlockInfo* info);
    // A runtime class with no messages: just a size and a destroy function.
    llvm::GlobalVariable* plainClass(const std::string& name, llvm::StructType* layout, llvm::Function* destroy);
    bool isBoxed(const sema::LocalInfo* local) const;
    llvm::GlobalVariable* boxClass(const sema::LocalInfo* local);
    // The box object holding the variable stored at `address`.
    llvm::Value* boxOf(llvm::Value* address);
    // Gives a boxed variable a new box; the frame lets go of the old one, which
    // closures made earlier still share.
    void allocateBox(const sema::LocalInfo* local);
    // Stack slots for a function's variables (a box pointer for boxed ones).
    void createSlots(const std::vector<sema::LocalInfo*>& locals);

    // Fields and variables by ownership. A weak or unowned one holds the pointer
    // with a weak count; reading it gives an owned (+1) strong reference.
    llvm::Value* loadHeld(llvm::Value* address, const Type* type, sema::Ownership ownership, const std::string& name);
    // Stores an owned value, letting go of the old one.
    void storeHeld(llvm::Value* address, llvm::Value* owned, const Type* type, sema::Ownership ownership);
    // Stores an owned value into memory that holds nothing yet (a new object's field).
    void initializeHeld(llvm::Value* address, llvm::Value* owned, const Type* type, sema::Ownership ownership);
    void dropHeld(llvm::Value* value, const Type* type, sema::Ownership ownership);
    void copyHeld(llvm::Value* value, const Type* type, sema::Ownership ownership);
    // The field an assignment target names (`x` inside the class, `p.x`), if any.
    const sema::FieldInfo* assignedField(const Node* target) const;
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
    // Value conversions the analyzer allowed implicitly: T -> T?, checked
    // conversions into subranges. Everything else goes through widen().
    llvm::Value* coerce(llvm::Value* value, const Type* from, const Type* to);
    llvm::Value* optionalPresent(llvm::Value* value, const Type* type);
    llvm::Value* optionalValue(llvm::Value* value, const Type* type);
    llvm::Value* makeOptional(llvm::Value* present, llvm::Value* value, const Type* type);
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

    // ---- compile time (codegenCompileTime.cpp) ----
    void emitThunk();
    // A compile-time value as code: an LLVM constant where possible (strings and
    // arrays become immortal read-only objects), otherwise built at run time.
    llvm::Value* materialize(const sema::ConstValue* value, const Type* type);
    llvm::Constant* constantOf(const sema::ConstValue& value, const Type* type);
    llvm::Value* emitCompileTimeSend(const Node* node, const sema::SendInfo& send);
    bool isCompileTimeContext(const sema::MessageInfo* message) const;

    // ---- phase 5: strings and containers (codegenContainers.cpp) ----
    llvm::Constant* stringLiteral(const std::string& text);
    llvm::Value* printString(llvm::Value* value, const Type* type);
    llvm::Value* emitStringSend(const Node* node, const sema::SendInfo& send);
    // `obj class` and the `Class` messages (codegenReflection.cpp); null if not one.
    llvm::Value* emitReflectionSend(const Node* node, const sema::SendInfo& send);
    // The class's BobClassMeta, or null below the `introspect` level.
    llvm::Constant* emitClassMeta(const sema::ClassInfo* info);
    // ---- C functions (codegenExtern.cpp) ----
    CPassing cPassing(const Type* type);
    CSignature cSignature(const std::vector<const Type*>& params, const Type* result);
    void applyCAttributes(llvm::Function* function, const CSignature& signature);
    void applyCAttributes(llvm::CallInst* call, const CSignature& signature);
    llvm::Value* toC(llvm::Value* value, const Type* type, const CPassing& passing);
    llvm::Value* fromC(llvm::Value* value, const Type* type, const CPassing& passing);
    // Calls `callee` (a C function pointer) with bob values; returns a bob value or null.
    llvm::Value* emitCCall(llvm::Value* callee, const CSignature& signature, const std::vector<llvm::Value*>& args);
    // A function with the C signature that calls `target` (bob ABI; `withEnvironment`: a null block environment first).
    llvm::Function* emitCThunk(const std::string& name, llvm::GlobalValue::LinkageTypes linkage, const CSignature& signature,
                               llvm::Function* target, bool withEnvironment);
    // A definition named `name`, taking over any declaration of that name already in the module.
    llvm::Function* cDefinition(const std::string& name, llvm::FunctionType* type, llvm::GlobalValue::LinkageTypes linkage);
    llvm::Function* cDeclaration(const std::string& name, const CSignature& signature);
    void emitExternImport(const sema::MessageInfo* message);
    void emitExport(const sema::MessageInfo* message);
    llvm::Value* emitCFunctionBlock(const sema::BlockInfo* info);
    llvm::Value* emitCFunctionInvoke(const Node* node, const sema::SendInfo& send);

    // Pointer and slice sends (codegenPointers.cpp); null if not one.
    llvm::Value* emitPointerSend(const Node* node, const sema::SendInfo& send);
    // Whether a field place (`p.x`, `r.a.b`) is reached through a volatilePointer; `align` is its alignment.
    bool placeAccess(const Node* place, llvm::Align& align) const;
    bool throughPointer(const Node* place) const;
    llvm::Function* fieldOffsetFunction();
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
    std::unordered_map<const sema::ClassInfo*, llvm::GlobalVariable*> protocolGlobals_;
    std::unordered_map<const sema::MessageInfo*, llvm::Function*> reflectionThunks_;
    llvm::Function* function_ = nullptr;
    const sema::MessageInfo* message_ = nullptr;
    const sema::BlockInfo* block_ = nullptr;
    llvm::Value* environment_ = nullptr;
    std::unordered_map<const sema::MessageInfo*, llvm::Function*> functions_;
    std::unordered_map<const sema::RecordInfo*, llvm::StructType*> structs_;
    std::unordered_map<const sema::RecordInfo*, std::vector<unsigned>> recordSlots_;
    std::unordered_map<const sema::LocalInfo*, llvm::AllocaInst*> slots_;
    struct StackSlot {
        llvm::AllocaInst* object;
        llvm::AllocaInst* built;
        const sema::ClassInfo* classInfo;
    };
    std::unordered_map<ast::NodeId, StackSlot> stackSlots_;
    std::unordered_map<const sema::BlockInfo*, llvm::StructType*> environmentTypes_;
    std::unordered_map<const sema::BlockInfo*, llvm::GlobalVariable*> environmentClasses_;
    std::unordered_map<const sema::LocalInfo*, llvm::GlobalVariable*> boxClasses_;

    std::unordered_map<const Type*, llvm::Function*> retainCallbacks_;
    std::unordered_map<const Type*, llvm::Function*> releaseCallbacks_;
    std::unordered_map<const Type*, llvm::Constant*> descriptors_;
    std::unordered_map<const sema::EnumInfo*, llvm::GlobalVariable*> enumValueTables_;
    std::unordered_map<const sema::EnumInfo*, llvm::GlobalVariable*> enumNameTables_;
    std::unordered_map<const sema::EnumInfo*, llvm::Function*> enumFinders_;
    std::map<std::string, llvm::Constant*> stringLiterals_;
    // Embedded arrays, one read-only object per compile-time value.
    std::map<const sema::ConstValue*, llvm::Constant*> constantArrays_;
    const sema::CompileTimeItem* evaluating_ = nullptr;
    std::string thunkName_;
    std::string triple_;

    struct LoopTargets {
        llvm::BasicBlock* breakTarget;
        llvm::BasicBlock* continueTarget;
    };
    std::vector<LoopTargets> loops_;
};

}
