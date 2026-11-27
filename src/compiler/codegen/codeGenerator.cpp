#include "codegen/codeGenerator.h"

#include "abi.h"
#include "objectModel.h"

#include <llvm/IR/Constants.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <vector>

namespace bob::codegen {

namespace {

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
    llvm::Value* fieldAddress(llvm::Value* object, const sema::FieldInfo* field);
    llvm::Value* selfValue();
    llvm::FunctionType* methodType(const sema::MessageInfo* message);

    // reference counting
    void retain(llvm::Value* value);
    void release(llvm::Value* value);
    bool producesOwned(const Node* node) const;
    llvm::Value* emitOwnedAs(const Node* expression, const Type* target);
    void storeReference(llvm::Value* address, llvm::Value* owned, const Type* type);
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
    std::vector<llvm::Value*> temporaries_;
    std::unordered_map<const sema::ClassInfo*, llvm::StructType*> objectTypes_;
    std::unordered_map<const sema::ClassInfo*, llvm::GlobalVariable*> classGlobals_;
    std::unordered_map<const sema::ClassInfo*, llvm::Function*> initFunctions_;
    std::unordered_map<const sema::ClassInfo*, llvm::Function*> newFunctions_;
    std::unordered_map<const sema::ClassInfo*, llvm::Function*> destroyFunctions_;
    std::unordered_map<std::string, llvm::GlobalVariable*> selectors_;
    llvm::Function* function_ = nullptr;
    const sema::MessageInfo* message_ = nullptr;
    const sema::BlockInfo* block_ = nullptr;
    llvm::Value* environment_ = nullptr;
    std::unordered_map<const sema::MessageInfo*, llvm::Function*> functions_;
    std::unordered_map<const sema::RecordInfo*, llvm::StructType*> structs_;
    std::unordered_map<const sema::LocalInfo*, llvm::AllocaInst*> slots_;

    struct LoopTargets {
        llvm::BasicBlock* breakTarget;
        llvm::BasicBlock* continueTarget;
    };
    std::vector<LoopTargets> loops_;
};

// ---------------------------------------------------------------------------
// module structure

std::unique_ptr<llvm::Module> Generator::run(const Unit* unit)
{
    module_ = std::make_unique<llvm::Module>(unit->name.empty() ? "bob" : unit->name, context_);
    module_->setDataLayout(dataLayout_);
    llvm::Type* ptr = builder_.getPtrTy();
    llvm::Type* voidType = builder_.getVoidTy();
    auto runtime = [&](const char* name, llvm::Type* result, std::vector<llvm::Type*> params) {
        return llvm::Function::Create(llvm::FunctionType::get(result, params, false), llvm::GlobalValue::ExternalLinkage, name,
                                      module_.get());
    };
    trapFunction_ = runtime(abi::trapSymbol, voidType, {builder_.getInt32Ty()});
    trapFunction_->setDoesNotReturn();
    trapFunction_->setDoesNotThrow();
    retainFunction_ = runtime("bobRetain", voidType, {ptr});
    releaseFunction_ = runtime("bobRelease", voidType, {ptr});
    allocFunction_ = runtime("bobAllocObject", ptr, {ptr});
    msgSendFunction_ = runtime("bob_msgSend", voidType, {});

    for (const auto& info : model_.classes) {
        declareClass(info.get());
    }
    for (const auto& message : model_.messages) {
        bool isInitializer = message->owner != nullptr && message.get() == message->owner->initializer;
        if (!isInitializer && !message->isRequired) {
            declareMessage(message.get());
        }
    }
    for (const auto& message : model_.messages) {
        if (functions_.count(message.get()) != 0) {
            emitMessage(message.get());
        }
    }
    for (const auto& info : model_.classes) {
        emitClass(info.get());
    }
    if (model_.program.main != nullptr) {
        emitEntryPoint();
    }
    return std::move(module_);
}

// ---------------------------------------------------------------------------
// classes and objects

// Object layout: { class*, refCount, flags, fields... } with inherited fields first.
llvm::StructType* Generator::objectType(const sema::ClassInfo* info)
{
    auto it = objectTypes_.find(info);
    if (it != objectTypes_.end()) {
        return it->second;
    }
    llvm::StructType* type = llvm::StructType::create(context_, "object." + info->name);
    objectTypes_[info] = type;
    std::vector<llvm::Type*> fields{builder_.getPtrTy(), builder_.getInt32Ty(), builder_.getInt32Ty()};
    for (const sema::FieldInfo& field : info->fields) {
        fields.push_back(llvmType(field.type));
    }
    type->setBody(fields);
    return type;
}

// Matches BobClass in src/shared/objectModel.h, followed by the vtable.
llvm::StructType* Generator::classInfoType(size_t vtableSize)
{
    llvm::Type* ptr = builder_.getPtrTy();
    return llvm::StructType::get(context_, {ptr, ptr, builder_.getInt64Ty(), ptr, ptr, builder_.getInt32Ty(), builder_.getInt32Ty(), ptr,
                                            llvm::ArrayType::get(ptr, vtableSize)});
}

// Selectors are unique strings; bob_msgSend compares them by address.
llvm::GlobalVariable* Generator::selectorGlobal(const std::string& selector)
{
    auto it = selectors_.find(selector);
    if (it != selectors_.end()) {
        return it->second;
    }
    llvm::Constant* text = llvm::ConstantDataArray::getString(context_, selector);
    auto global = new llvm::GlobalVariable(*module_, text->getType(), true, llvm::GlobalValue::PrivateLinkage, text, "bob.sel." + selector);
    selectors_[selector] = global;
    return global;
}

llvm::FunctionType* Generator::methodType(const sema::MessageInfo* message)
{
    std::vector<llvm::Type*> params;
    if (message->isInstance) {
        params.push_back(builder_.getPtrTy());
        params.push_back(builder_.getPtrTy());
    }
    for (const sema::LocalInfo* param : message->params) {
        params.push_back(llvmType(param->type));
    }
    return llvm::FunctionType::get(llvmType(message->returnType), params, false);
}

void Generator::declareClass(const sema::ClassInfo* info)
{
    if (info->isProtocol) {
        return;
    }
    objectType(info);
    llvm::Type* ptr = builder_.getPtrTy();
    auto linkage = llvm::GlobalValue::InternalLinkage;
    classGlobals_[info] = new llvm::GlobalVariable(*module_, classInfoType(info->vtable.size()), false, linkage, nullptr, "class." + info->name);
    destroyFunctions_[info] = llvm::Function::Create(llvm::FunctionType::get(builder_.getVoidTy(), {ptr}, false), linkage,
                                                     "bob." + info->name + ".destroy", module_.get());
    initFunctions_[info] = llvm::Function::Create(llvm::FunctionType::get(builder_.getVoidTy(), {ptr}, false), linkage,
                                                  "bob." + info->name + ".init", module_.get());
    newFunctions_[info] = llvm::Function::Create(llvm::FunctionType::get(ptr, {}, false), linkage, "bob." + info->name + ".new", module_.get());
}

llvm::Value* Generator::fieldAddress(llvm::Value* object, const sema::FieldInfo* field)
{
    return builder_.CreateStructGEP(objectType(field->owner), object, field->index + 3, field->name);
}

llvm::Value* Generator::selfValue()
{
    return builder_.CreateLoad(builder_.getPtrTy(), addressOf(message_->self), "self");
}

void Generator::emitClass(const sema::ClassInfo* info)
{
    if (info->isProtocol) {
        return;
    }
    llvm::Type* ptr = builder_.getPtrTy();

    // destroy: release the object's reference fields
    function_ = destroyFunctions_.at(info);
    builder_.SetInsertPoint(llvm::BasicBlock::Create(context_, "entry", function_));
    for (const sema::FieldInfo& field : info->fields) {
        if (sema::isReference(field.type)) {
            release(builder_.CreateLoad(ptr, fieldAddress(function_->getArg(0), &field)));
        }
    }
    builder_.CreateRetVoid();

    // init: the superclass's initializers, then this class's own
    beginFunction(initFunctions_.at(info), info->initializer, info->initializer->locals);
    llvm::Value* object = function_->getArg(0);
    if (info->superclass != nullptr) {
        builder_.CreateCall(initFunctions_.at(info->superclass), {object});
    }
    for (const sema::FieldInfo& field : info->fields) {
        if (field.owner != info || field.initializer == nullptr) {
            continue;
        }
        size_t mark = temporaries_.size();
        llvm::Value* value = emitOwnedAs(field.initializer, field.type);
        builder_.CreateStore(value, fieldAddress(object, &field));
        flushTemporaries(mark);
    }
    emitReturn(nullptr);

    // new: allocate (zeroed, refCount 1) and initialize
    function_ = newFunctions_.at(info);
    builder_.SetInsertPoint(llvm::BasicBlock::Create(context_, "entry", function_));
    llvm::Value* created = builder_.CreateCall(allocFunction_, {classGlobals_.at(info)}, "object");
    builder_.CreateCall(initFunctions_.at(info), {created});
    builder_.CreateRet(created);

    // class info: header, method table for bob_msgSend, vtable
    std::vector<llvm::Constant*> entries;
    llvm::StructType* entryType = llvm::StructType::get(context_, {ptr, ptr});
    for (const sema::MessageInfo* method : info->methodList) {
        auto function = functions_.find(method);
        if (function != functions_.end()) {
            entries.push_back(llvm::ConstantStruct::get(entryType, {selectorGlobal(method->selector), function->second}));
        }
    }
    llvm::ArrayType* tableType = llvm::ArrayType::get(entryType, entries.size());
    auto table = new llvm::GlobalVariable(*module_, tableType, true, llvm::GlobalValue::PrivateLinkage,
                                          llvm::ConstantArray::get(tableType, entries), "methods." + info->name);
    std::vector<llvm::Constant*> vtable;
    for (const sema::MessageInfo* method : info->vtable) {
        auto function = method != nullptr ? functions_.find(method) : functions_.end();
        vtable.push_back(function != functions_.end() ? static_cast<llvm::Constant*>(function->second)
                                                      : llvm::ConstantPointerNull::get(builder_.getPtrTy()));
    }
    llvm::StructType* type = classInfoType(info->vtable.size());
    llvm::Constant* name = builder_.CreateGlobalString(info->name, "name." + info->name, 0, module_.get());
    uint64_t size = module_->getDataLayout().getTypeAllocSize(objectType(info));
    llvm::Constant* superclass = info->superclass != nullptr ? static_cast<llvm::Constant*>(classGlobals_.at(info->superclass))
                                                             : llvm::ConstantPointerNull::get(builder_.getPtrTy());
    classGlobals_.at(info)->setInitializer(llvm::ConstantStruct::get(
        type, {superclass, name, builder_.getInt64(size), destroyFunctions_.at(info), table, builder_.getInt32(static_cast<uint32_t>(entries.size())),
               builder_.getInt32(0), llvm::ConstantPointerNull::get(builder_.getPtrTy()),
               llvm::ConstantArray::get(llvm::ArrayType::get(ptr, vtable.size()), vtable)}));
}

// ---------------------------------------------------------------------------
// reference counting: variables and fields own their reference; parameters are
// borrowed; call results are owned. Owned values used only for the duration
// of a statement are "temporaries", released when the statement ends.

void Generator::retain(llvm::Value* value)
{
    if (!llvm::isa<llvm::ConstantPointerNull>(value)) {
        builder_.CreateCall(retainFunction_, {value});
    }
}

void Generator::release(llvm::Value* value)
{
    if (!llvm::isa<llvm::ConstantPointerNull>(value)) {
        builder_.CreateCall(releaseFunction_, {value});
    }
}

bool Generator::producesOwned(const Node* node) const
{
    if (!sema::isReference(model_.typeOf(node))) {
        return false;
    }
    if (node->kind == NodeKind::identifier) {
        if (model_.selfFieldRefs.count(node->id) != 0) {
            return true;
        }
        const sema::LocalInfo* local = model_.localOf(node);
        return local != nullptr && local->captured;
    }
    if (node->kind == NodeKind::selfExpr) {
        const sema::LocalInfo* self = model_.localOf(node);
        return self != nullptr && self->captured;
    }
    if (node->kind == NodeKind::fieldAccess) {
        return true;
    }
    if (node->kind == NodeKind::attributedExpr) {
        return producesOwned(static_cast<const AttributedExpr*>(node)->expression);
    }
    return model_.sendOf(node) != nullptr;
}

llvm::Value* Generator::emitOwnedAs(const Node* expression, const Type* target)
{
    llvm::Value* value = emit(expression);
    if (sema::isReference(model_.typeOf(expression)) && !producesOwned(expression)) {
        retain(value);
    }
    return widen(value, model_.typeOf(expression), target);
}

void Generator::storeReference(llvm::Value* address, llvm::Value* owned, const Type* type)
{
    if (!sema::isReference(type)) {
        builder_.CreateStore(owned, address);
        return;
    }
    llvm::Value* old = builder_.CreateLoad(builder_.getPtrTy(), address, "old");
    builder_.CreateStore(owned, address);
    release(old);
}

void Generator::flushTemporaries(size_t mark)
{
    if (builder_.GetInsertBlock()->hasTerminator()) {
        temporaries_.resize(std::min(mark, temporaries_.size()));
        return;
    }
    while (temporaries_.size() > mark) {
        release(temporaries_.back());
        temporaries_.pop_back();
    }
}

void Generator::releaseLocals()
{
    for (const auto& [local, slot] : slots_) {
        if (sema::isReference(local->type) && !local->isParameter) {
            release(builder_.CreateLoad(builder_.getPtrTy(), slot));
        }
    }
}

// Every exit from a function: release pending temporaries (they stay listed for
// other paths) and owned locals, then return.
void Generator::emitReturn(llvm::Value* value)
{
    for (auto it = temporaries_.rbegin(); it != temporaries_.rend(); ++it) {
        release(*it);
    }
    releaseLocals();
    if (value != nullptr) {
        builder_.CreateRet(value);
    }
    else {
        builder_.CreateRetVoid();
    }
}

void Generator::beginFunction(llvm::Function* function, const sema::MessageInfo* message, const std::vector<sema::LocalInfo*>& locals)
{
    function_ = function;
    message_ = message;
    block_ = nullptr;
    environment_ = nullptr;
    slots_.clear();
    loops_.clear();
    temporaries_.clear();
    builder_.SetInsertPoint(llvm::BasicBlock::Create(context_, "entry", function_));
    for (const sema::LocalInfo* local : locals) {
        if (sema::isValueType(local->type)) {
            llvm::AllocaInst* slot = builder_.CreateAlloca(llvmType(local->type), nullptr, local->name);
            slots_[local] = slot;
            if (sema::isReference(local->type) && !local->isParameter) {
                builder_.CreateStore(llvm::ConstantPointerNull::get(builder_.getPtrTy()), slot);
            }
        }
    }
}

llvm::Type* Generator::llvmType(const Type* type)
{
    switch (type->kind) {
    case TypeKind::voidType:
        return builder_.getVoidTy();
    case TypeKind::boolType:
        return builder_.getInt1Ty();
    case TypeKind::integer:
        return builder_.getIntNTy(type->bits);
    case TypeKind::real:
        return type->bits == 32 ? builder_.getFloatTy() : builder_.getDoubleTy();
    case TypeKind::record:
        return structType(type->record);
    case TypeKind::block:
        return blockValueType();
    case TypeKind::classType:
    case TypeKind::optional:
    case TypeKind::nilType:
    case TypeKind::blockToInfer:
    case TypeKind::placeholder:
    case TypeKind::meta:
    case TypeKind::error:
        return builder_.getPtrTy();
    }
    return builder_.getPtrTy();
}

llvm::StructType* Generator::structType(const sema::RecordInfo* record)
{
    auto it = structs_.find(record);
    if (it != structs_.end()) {
        return it->second;
    }
    llvm::StructType* type = llvm::StructType::create(context_, "record." + record->name);
    structs_[record] = type;
    std::vector<llvm::Type*> fields;
    for (const sema::FieldInfo& field : record->fields) {
        fields.push_back(llvmType(field.type));
    }
    type->setBody(fields, record->packed);
    return type;
}

void Generator::declareMessage(const sema::MessageInfo* message)
{
    llvm::Function* function = llvm::Function::Create(methodType(message), llvm::GlobalValue::InternalLinkage, message->symbolName, module_.get());
    unsigned offset = message->isInstance ? 2 : 0;
    if (message->isInstance) {
        function->getArg(0)->setName("self");
        function->getArg(1)->setName("selector");
    }
    for (size_t i = 0; i < message->params.size(); ++i) {
        function->getArg(static_cast<unsigned>(i) + offset)->setName(message->params[i]->name);
    }
    functions_[message] = function;
}

void Generator::emitMessage(const sema::MessageInfo* message)
{
    beginFunction(functions_.at(message), message, message->locals);
    unsigned offset = 0;
    if (message->isInstance) {
        builder_.CreateStore(function_->getArg(0), slots_.at(message->self));
        offset = 2;
    }
    for (size_t i = 0; i < message->params.size(); ++i) {
        auto slot = slots_.find(message->params[i]);
        if (slot != slots_.end()) {
            builder_.CreateStore(function_->getArg(static_cast<unsigned>(i) + offset), slot->second);
        }
    }

    auto body = nodeCast<StatementGroup>(message->decl->body);
    if (body != nullptr) {
        emitStatements(body->statements);
    }
    if (!builder_.GetInsertBlock()->hasTerminator()) {
        if (message->returnType->kind == TypeKind::voidType) {
            emitReturn(nullptr);
        }
        else {
            builder_.CreateUnreachable();
        }
    }
}

void Generator::emitEntryPoint()
{
    auto type = llvm::FunctionType::get(builder_.getInt32Ty(), {}, false);
    llvm::Function* entry = llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage, abi::mainSymbol, module_.get());
    builder_.SetInsertPoint(llvm::BasicBlock::Create(context_, "entry", entry));
    std::vector<llvm::Value*> args;
    for (const sema::LocalInfo* param : model_.program.main->params) {
        args.push_back(llvm::Constant::getNullValue(llvmType(param->type)));
    }
    builder_.CreateRet(builder_.CreateCall(functions_.at(model_.program.main), args));
}

// ---------------------------------------------------------------------------
// statements

void Generator::ensureLiveBlock()
{
    if (builder_.GetInsertBlock()->hasTerminator()) {
        builder_.SetInsertPoint(llvm::BasicBlock::Create(context_, "dead", function_));
    }
}

void Generator::emitStatements(const std::vector<Node*>& statements)
{
    for (const Node* statement : statements) {
        emitStatement(statement);
    }
}

void Generator::emitGroup(const Node* group)
{
    if (auto statements = nodeCast<StatementGroup>(group)) {
        emitStatements(statements->statements);
    }
}

void Generator::store(const sema::LocalInfo* local, llvm::Value* value)
{
    builder_.CreateStore(value, addressOf(local));
}

// A variable is either owned by the current function (an alloca) or captured
// from an enclosing one, in which case the block's environment holds its address.
llvm::Value* Generator::addressOf(const sema::LocalInfo* local)
{
    auto slot = slots_.find(local);
    if (slot != slots_.end()) {
        return slot->second;
    }
    const auto& captures = block_->captures;
    auto it = std::find(captures.begin(), captures.end(), local);
    unsigned index = static_cast<unsigned>(it - captures.begin());
    std::vector<llvm::Type*> fields(captures.size(), builder_.getPtrTy());
    llvm::StructType* environmentType = llvm::StructType::get(context_, fields);
    llvm::Value* field = builder_.CreateStructGEP(environmentType, environment_, index);
    return builder_.CreateLoad(builder_.getPtrTy(), field, local->name + ".ref");
}

llvm::AllocaInst* Generator::entryAlloca(llvm::Type* type, const std::string& name)
{
    llvm::BasicBlock& entry = function_->getEntryBlock();
    llvm::IRBuilder<> allocaBuilder(&entry, entry.begin());
    return allocaBuilder.CreateAlloca(type, nullptr, name);
}

void Generator::emitStatement(const Node* statement)
{
    ensureLiveBlock();
    size_t mark = temporaries_.size();
    switch (statement->kind) {
    case NodeKind::variableDecl: {
        auto decl = static_cast<const VariableDecl*>(statement);
        const sema::LocalInfo* local = model_.localOf(decl);
        llvm::Value* value = decl->initializer != nullptr ? emitOwnedAs(decl->initializer, local->type)
                                                          : llvm::Constant::getNullValue(llvmType(local->type));
        storeReference(addressOf(local), value, local->type);
        break;
    }
    case NodeKind::assignment: {
        auto assignment = static_cast<const Assignment*>(statement);
        const sema::LocalInfo* local = model_.localOf(assignment->target);
        if (local != nullptr) {
            llvm::Value* value = emitOwnedAs(assignment->value, local->type);
            storeReference(addressOf(local), value, local->type);
        }
        else {
            const Type* type = model_.typeOf(assignment->target);
            llvm::Value* value = emitOwnedAs(assignment->value, type);
            storeReference(emitAddress(assignment->target), value, type);
        }
        break;
    }
    case NodeKind::expressionStmt: {
        const Node* expression = static_cast<const ExpressionStmt*>(statement)->expression;
        emitAs(expression, model_.typeOf(expression));
        break;
    }
    case NodeKind::returnStmt: {
        auto ret = static_cast<const ReturnStmt*>(statement);
        emitReturn(ret->values.empty() ? nullptr : emitOwnedAs(ret->values[0], message_->returnType));
        break;
    }
    case NodeKind::ifStmt:
        emitIf(static_cast<const IfStmt*>(statement));
        break;
    case NodeKind::whileStmt: {
        auto loop = static_cast<const WhileStmt*>(statement);
        llvm::BasicBlock* condition = llvm::BasicBlock::Create(context_, "while.cond", function_);
        llvm::BasicBlock* body = llvm::BasicBlock::Create(context_, "while.body", function_);
        llvm::BasicBlock* exit = llvm::BasicBlock::Create(context_, "while.end", function_);
        builder_.CreateBr(condition);
        builder_.SetInsertPoint(condition);
        llvm::Value* test = emitAs(loop->condition, model_.types.boolType());
        flushTemporaries(mark);
        builder_.CreateCondBr(test, body, exit);
        builder_.SetInsertPoint(body);
        loops_.push_back({exit, condition});
        emitGroup(loop->body);
        loops_.pop_back();
        if (!builder_.GetInsertBlock()->hasTerminator()) {
            builder_.CreateBr(condition);
        }
        builder_.SetInsertPoint(exit);
        break;
    }
    case NodeKind::loopStmt: {
        auto loop = static_cast<const LoopStmt*>(statement);
        llvm::BasicBlock* body = llvm::BasicBlock::Create(context_, "loop.body", function_);
        llvm::BasicBlock* exit = llvm::BasicBlock::Create(context_, "loop.end", function_);
        builder_.CreateBr(body);
        builder_.SetInsertPoint(body);
        loops_.push_back({exit, body});
        emitGroup(loop->body);
        loops_.pop_back();
        if (!builder_.GetInsertBlock()->hasTerminator()) {
            builder_.CreateBr(body);
        }
        builder_.SetInsertPoint(exit);
        break;
    }
    case NodeKind::forStmt:
        emitFor(static_cast<const ForStmt*>(statement));
        break;
    case NodeKind::breakStmt:
        builder_.CreateBr(loops_.back().breakTarget);
        break;
    case NodeKind::continueStmt:
        builder_.CreateBr(loops_.back().continueTarget);
        break;
    case NodeKind::caseStmt:
        emitCase(static_cast<const CaseStmt*>(statement));
        break;
    case NodeKind::statementGroup:
        emitGroup(statement);
        break;
    default:
        break;
    }
    flushTemporaries(mark);
}

void Generator::emitIf(const IfStmt* statement)
{
    llvm::BasicBlock* thenBlock = llvm::BasicBlock::Create(context_, "if.then", function_);
    llvm::BasicBlock* merge = llvm::BasicBlock::Create(context_, "if.end", function_);
    llvm::BasicBlock* elseBlock = statement->elseBody != nullptr ? llvm::BasicBlock::Create(context_, "if.else", function_) : merge;
    size_t mark = temporaries_.size();
    llvm::Value* test = emitAs(statement->condition, model_.types.boolType());
    flushTemporaries(mark);
    builder_.CreateCondBr(test, thenBlock, elseBlock);

    builder_.SetInsertPoint(thenBlock);
    emitGroup(statement->thenBody);
    if (!builder_.GetInsertBlock()->hasTerminator()) {
        builder_.CreateBr(merge);
    }
    if (statement->elseBody != nullptr) {
        builder_.SetInsertPoint(elseBlock);
        if (statement->elseBody->kind == NodeKind::ifStmt) {
            emitIf(static_cast<const IfStmt*>(statement->elseBody));
        }
        else {
            emitGroup(statement->elseBody);
        }
        if (!builder_.GetInsertBlock()->hasTerminator()) {
            builder_.CreateBr(merge);
        }
    }
    builder_.SetInsertPoint(merge);
}

void Generator::emitFor(const ForStmt* statement)
{
    auto interval = static_cast<const KeywordSend*>(statement->iterable);
    auto body = static_cast<const StatementGroup*>(statement->body);
    emitCountingLoop(model_.localOf(statement), interval->receiver, interval->arguments[0], body->statements);
}

// Visits low..high inclusive (`for i in a to: b`, `a to: b do: { :i | }`). The
// exit test happens before the increment, so `high` may be the type's maximum.
void Generator::emitCountingLoop(const sema::LocalInfo* local, const Node* lowNode, const Node* highNode, const std::vector<Node*>& statements)
{
    const Type* type = local->type;
    llvm::Value* low = emitAs(lowNode, type);
    llvm::Value* high = emitAs(highNode, type);

    llvm::BasicBlock* body = llvm::BasicBlock::Create(context_, "for.body", function_);
    llvm::BasicBlock* step = llvm::BasicBlock::Create(context_, "for.step", function_);
    llvm::BasicBlock* increment = llvm::BasicBlock::Create(context_, "for.inc", function_);
    llvm::BasicBlock* exit = llvm::BasicBlock::Create(context_, "for.end", function_);
    store(local, low);
    llvm::Value* nonEmpty = type->isSigned ? builder_.CreateICmpSLE(low, high) : builder_.CreateICmpULE(low, high);
    builder_.CreateCondBr(nonEmpty, body, exit);

    builder_.SetInsertPoint(body);
    loops_.push_back({exit, step});
    emitStatements(statements);
    loops_.pop_back();
    if (!builder_.GetInsertBlock()->hasTerminator()) {
        builder_.CreateBr(step);
    }

    builder_.SetInsertPoint(step);
    llvm::Value* current = builder_.CreateLoad(llvmType(type), addressOf(local));
    builder_.CreateCondBr(builder_.CreateICmpEQ(current, high), exit, increment);
    builder_.SetInsertPoint(increment);
    store(local, builder_.CreateAdd(current, llvm::ConstantInt::get(llvmType(type), 1)));
    builder_.CreateBr(body);

    builder_.SetInsertPoint(exit);
}

void Generator::emitCase(const CaseStmt* statement)
{
    const Type* type = model_.typeOf(statement->subject);
    llvm::Value* subject = emit(statement->subject);
    llvm::Type* llvmSubject = llvmType(type);
    llvm::BasicBlock* merge = llvm::BasicBlock::Create(context_, "case.end", function_);

    for (const Node* node : statement->entries) {
        auto entry = nodeCast<CaseEntry>(node);
        auto ranges = model_.caseRanges.find(entry->id);
        llvm::Value* matches = builder_.getFalse();
        for (const sema::CaseRange& range : ranges->second) {
            llvm::Value* low = llvm::ConstantInt::get(llvmSubject, range.low, type->isSigned);
            llvm::Value* high = llvm::ConstantInt::get(llvmSubject, range.high, type->isSigned);
            llvm::Value* test = nullptr;
            if (range.low == range.high) {
                test = builder_.CreateICmpEQ(subject, low);
            }
            else if (type->isSigned) {
                test = builder_.CreateAnd(builder_.CreateICmpSGE(subject, low), builder_.CreateICmpSLE(subject, high));
            }
            else {
                test = builder_.CreateAnd(builder_.CreateICmpUGE(subject, low), builder_.CreateICmpULE(subject, high));
            }
            matches = builder_.CreateOr(matches, test);
        }
        llvm::BasicBlock* bodyBlock = llvm::BasicBlock::Create(context_, "case.entry", function_);
        llvm::BasicBlock* next = llvm::BasicBlock::Create(context_, "case.next", function_);
        builder_.CreateCondBr(matches, bodyBlock, next);
        builder_.SetInsertPoint(bodyBlock);
        emitStatements(static_cast<const BlockLiteral*>(entry->body)->statements);
        if (!builder_.GetInsertBlock()->hasTerminator()) {
            builder_.CreateBr(merge);
        }
        builder_.SetInsertPoint(next);
    }
    emitGroup(statement->elseBody);
    if (!builder_.GetInsertBlock()->hasTerminator()) {
        builder_.CreateBr(merge);
    }
    builder_.SetInsertPoint(merge);
}

// ---------------------------------------------------------------------------
// expressions

// A borrowed use: an owned reference result becomes a temporary, released when
// the statement ends.
llvm::Value* Generator::emitAs(const Node* expression, const Type* target)
{
    llvm::Value* value = emit(expression);
    if (value != nullptr && sema::isReference(model_.typeOf(expression)) && producesOwned(expression)) {
        temporaries_.push_back(value);
    }
    return widen(value, model_.typeOf(expression), target);
}

llvm::Value* Generator::widen(llvm::Value* value, const Type* from, const Type* to)
{
    if (from == to || to == nullptr || from == nullptr) {
        return value;
    }
    if (sema::isInteger(from) && sema::isInteger(to)) {
        return from->isSigned ? builder_.CreateSExt(value, llvmType(to)) : builder_.CreateZExt(value, llvmType(to));
    }
    if (sema::isReal(from) && sema::isReal(to)) {
        return builder_.CreateFPExt(value, llvmType(to));
    }
    return value;
}

llvm::Value* Generator::emitAddress(const Node* expression)
{
    if (expression->kind == NodeKind::identifier) {
        auto field = model_.selfFieldRefs.find(expression->id);
        if (field != model_.selfFieldRefs.end()) {
            return fieldAddress(selfValue(), field->second);
        }
        const sema::LocalInfo* local = model_.localOf(expression);
        return local != nullptr ? addressOf(local) : nullptr;
    }
    if (auto access = nodeCast<FieldAccess>(expression)) {
        if (sema::isClass(model_.typeOf(access->receiver))) {
            return fieldAddress(emitAs(access->receiver, nullptr), model_.fieldOf(access));
        }
        llvm::Value* base = emitAddress(access->receiver);
        if (base == nullptr) {
            return nullptr;
        }
        const Type* recordType = model_.typeOf(access->receiver);
        return builder_.CreateStructGEP(structType(recordType->record), base, model_.fieldOf(access)->index, access->field);
    }
    return nullptr;
}

llvm::Value* Generator::emitLiteral(const Node* node)
{
    const Type* type = model_.typeOf(node);
    if (auto integer = nodeCast<IntegerLiteral>(node)) {
        if (sema::isReal(type)) {
            double value = static_cast<double>(integer->value);
            return llvm::ConstantFP::get(llvmType(type), integer->negative ? -value : value);
        }
        uint64_t bits = integer->negative ? (~integer->value + 1) : integer->value;
        return llvm::ConstantInt::get(llvmType(type), bits, type->isSigned);
    }
    if (auto real = nodeCast<RealLiteral>(node)) {
        return llvm::ConstantFP::get(llvmType(type), real->value);
    }
    return builder_.getInt1(static_cast<const BoolLiteral*>(node)->value);
}

llvm::Value* Generator::emit(const Node* expression)
{
    switch (expression->kind) {
    case NodeKind::integerLiteral:
    case NodeKind::realLiteral:
    case NodeKind::boolLiteral:
        return emitLiteral(expression);
    case NodeKind::identifier:
    case NodeKind::selfExpr: {
        const Type* type = model_.typeOf(expression);
        llvm::Value* value = nullptr;
        if (model_.selfFieldRefs.count(expression->id) != 0) {
            value = builder_.CreateLoad(llvmType(type), emitAddress(expression));
        }
        else {
            const sema::LocalInfo* local = model_.localOf(expression);
            if (local == nullptr || !sema::isValueType(local->type)) {
                return nullptr;
            }
            value = builder_.CreateLoad(llvmType(local->type), addressOf(local), local->name);
        }
        // Loads that something else could overwrite mid-statement (fields, captured
        // variables) are retained so the value stays alive while it's used.
        if (producesOwned(expression)) {
            retain(value);
        }
        return value;
    }
    case NodeKind::nilLiteral:
        return llvm::ConstantPointerNull::get(builder_.getPtrTy());
    case NodeKind::blockLiteral:
        return emitBlockLiteral(static_cast<const BlockLiteral*>(expression));
    case NodeKind::fieldAccess: {
        auto access = static_cast<const FieldAccess*>(expression);
        const Type* type = model_.typeOf(access);
        if (llvm::Value* address = emitAddress(access)) {
            llvm::Value* value = builder_.CreateLoad(llvmType(type), address, access->field);
            if (sema::isReference(type)) {
                retain(value);
            }
            return value;
        }
        return builder_.CreateExtractValue(emit(access->receiver), {model_.fieldOf(access)->index}, access->field);
    }
    case NodeKind::attributedExpr:
        return emit(static_cast<const AttributedExpr*>(expression)->expression);
    case NodeKind::unarySend:
    case NodeKind::binarySend:
    case NodeKind::keywordSend: {
        const sema::SendInfo* send = model_.sendOf(expression);
        return send != nullptr ? emitSend(expression, *send) : nullptr;
    }
    default:
        return nullptr;
    }
}

llvm::Value* Generator::emitSend(const Node* node, const sema::SendInfo& send)
{
    const Type* type = send.operandType;
    switch (send.kind) {
    case SendKind::arithmetic:
    case SendKind::wrapArithmetic:
    case SendKind::saturateArithmetic: {
        const Node* lhs = nullptr;
        const Node* rhs = nullptr;
        if (auto binary = nodeCast<BinarySend>(node)) {
            lhs = binary->receiver;
            rhs = binary->argument;
        }
        else {
            auto keyword = static_cast<const KeywordSend*>(node);
            lhs = keyword->receiver;
            rhs = keyword->arguments[0];
        }
        OverflowMode mode = send.kind == SendKind::wrapArithmetic       ? OverflowMode::wrap
                            : send.kind == SendKind::saturateArithmetic ? OverflowMode::saturate
                                                                        : send.overflow;
        return arithmetic(send.op[0], emitAs(lhs, type), emitAs(rhs, type), type, mode);
    }
    case SendKind::bitwise: {
        auto binary = static_cast<const BinarySend*>(node);
        llvm::Value* a = emitAs(binary->receiver, type);
        llvm::Value* b = emitAs(binary->argument, type);
        return send.op == "&" ? builder_.CreateAnd(a, b) : send.op == "|" ? builder_.CreateOr(a, b) : builder_.CreateXor(a, b);
    }
    case SendKind::comparison: {
        auto binary = static_cast<const BinarySend*>(node);
        return compare(send.op, emitAs(binary->receiver, type), emitAs(binary->argument, type), type);
    }
    case SendKind::logicalAnd:
    case SendKind::logicalOr: {
        auto binary = static_cast<const BinarySend*>(node);
        bool isAnd = send.kind == SendKind::logicalAnd;
        llvm::Value* left = emitAs(binary->receiver, model_.types.boolType());
        llvm::BasicBlock* leftBlock = builder_.GetInsertBlock();
        llvm::BasicBlock* rightBlock = llvm::BasicBlock::Create(context_, isAnd ? "and.rhs" : "or.rhs", function_);
        llvm::BasicBlock* merge = llvm::BasicBlock::Create(context_, isAnd ? "and.end" : "or.end", function_);
        if (isAnd) {
            builder_.CreateCondBr(left, rightBlock, merge);
        }
        else {
            builder_.CreateCondBr(left, merge, rightBlock);
        }
        builder_.SetInsertPoint(rightBlock);
        size_t mark = temporaries_.size();
        llvm::Value* right = emitAs(binary->argument, model_.types.boolType());
        flushTemporaries(mark);
        rightBlock = builder_.GetInsertBlock();
        builder_.CreateBr(merge);
        builder_.SetInsertPoint(merge);
        llvm::PHINode* phi = builder_.CreatePHI(builder_.getInt1Ty(), 2);
        phi->addIncoming(builder_.getInt1(!isAnd), leftBlock);
        phi->addIncoming(right, rightBlock);
        return phi;
    }
    case SendKind::negate:
        return negate(emitAs(static_cast<const UnarySend*>(node)->receiver, type), type, send.overflow);
    case SendKind::absolute: {
        llvm::Value* value = emitAs(static_cast<const UnarySend*>(node)->receiver, type);
        if (sema::isReal(type)) {
            return builder_.CreateUnaryIntrinsic(llvm::Intrinsic::fabs, value);
        }
        if (!type->isSigned) {
            return value;
        }
        llvm::Value* isNegative = builder_.CreateICmpSLT(value, llvm::ConstantInt::get(value->getType(), 0));
        llvm::BasicBlock* before = builder_.GetInsertBlock();
        llvm::BasicBlock* negative = llvm::BasicBlock::Create(context_, "abs.neg", function_);
        llvm::BasicBlock* merge = llvm::BasicBlock::Create(context_, "abs.end", function_);
        builder_.CreateCondBr(isNegative, negative, merge);
        builder_.SetInsertPoint(negative);
        llvm::Value* negated = negate(value, type, send.overflow);
        negative = builder_.GetInsertBlock();
        builder_.CreateBr(merge);
        builder_.SetInsertPoint(merge);
        llvm::PHINode* phi = builder_.CreatePHI(value->getType(), 2);
        phi->addIncoming(value, before);
        phi->addIncoming(negated, negative);
        return phi;
    }
    case SendKind::boolNot:
        return builder_.CreateNot(emitAs(static_cast<const UnarySend*>(node)->receiver, type));
    case SendKind::conversion:
        return convert(emitAs(static_cast<const UnarySend*>(node)->receiver, type), type, send.resultType);
    case SendKind::minimum:
    case SendKind::maximum: {
        auto keyword = static_cast<const KeywordSend*>(node);
        llvm::Value* a = emitAs(keyword->receiver, type);
        llvm::Value* b = emitAs(keyword->arguments[0], type);
        llvm::Value* aIsLess = compare("<", a, b, type);
        return send.kind == SendKind::minimum ? builder_.CreateSelect(aIsLess, a, b) : builder_.CreateSelect(aIsLess, b, a);
    }
    case SendKind::shift: {
        auto binary = static_cast<const BinarySend*>(node);
        return shift(send.op == "shl", emitAs(binary->receiver, type), emit(binary->argument), type, model_.typeOf(binary->argument),
                     send.overflow);
    }
    case SendKind::staticCall: {
        std::vector<llvm::Value*> args;
        if (auto keyword = nodeCast<KeywordSend>(node)) {
            for (size_t i = 0; i < keyword->arguments.size(); ++i) {
                args.push_back(emitAs(keyword->arguments[i], send.target->params[i]->type));
            }
        }
        return builder_.CreateCall(functions_.at(send.target), args);
    }
    case SendKind::recordConstructor: {
        auto keyword = static_cast<const KeywordSend*>(node);
        llvm::Value* value = llvm::UndefValue::get(structType(send.record));
        for (size_t i = 0; i < keyword->arguments.size(); ++i) {
            value = builder_.CreateInsertValue(value, emitAs(keyword->arguments[i], send.record->fields[i].type), {static_cast<unsigned>(i)});
        }
        return value;
    }
    case SendKind::blockInvoke: {
        if (auto unary = nodeCast<UnarySend>(node)) {
            return emitInvoke(unary->receiver, {}, send.operandType);
        }
        auto keyword = static_cast<const KeywordSend*>(node);
        return emitInvoke(keyword->receiver, keyword->arguments, send.operandType);
    }
    case SendKind::inlineIf:
        return emitInlinedIf(static_cast<const KeywordSend*>(node), send);
    case SendKind::inlineToDo: {
        auto keyword = static_cast<const KeywordSend*>(node);
        auto literal = static_cast<const BlockLiteral*>(keyword->arguments[1]);
        emitCountingLoop(model_.blockOf(literal)->params[0], keyword->receiver, keyword->arguments[0], literal->statements);
        return nullptr;
    }
    case SendKind::inlineTimesRepeat:
        emitTimesRepeat(static_cast<const KeywordSend*>(node), type);
        return nullptr;
    case SendKind::interval:
        return nullptr;
    case SendKind::newObject:
        return builder_.CreateCall(newFunctions_.at(send.classInfo), {}, "new");
    case SendKind::classStaticCall: {
        std::vector<llvm::Value*> args;
        if (auto keyword = nodeCast<KeywordSend>(node)) {
            for (size_t i = 0; i < keyword->arguments.size(); ++i) {
                args.push_back(emitAs(keyword->arguments[i], send.target->params[i]->type));
            }
        }
        return builder_.CreateCall(functions_.at(send.target), args);
    }
    case SendKind::directMethodCall:
    case SendKind::superCall:
    case SendKind::vtableCall:
    case SendKind::dynamicSend: {
        const Node* receiver = nullptr;
        std::vector<const Node*> arguments;
        if (auto unary = nodeCast<UnarySend>(node)) {
            receiver = unary->receiver;
        }
        else if (auto binary = nodeCast<BinarySend>(node)) {
            receiver = binary->receiver;
            arguments.push_back(binary->argument);
        }
        else {
            auto keyword = static_cast<const KeywordSend*>(node);
            receiver = keyword->receiver;
            arguments.assign(keyword->arguments.begin(), keyword->arguments.end());
        }
        llvm::Value* self = send.kind == SendKind::superCall ? selfValue() : emitAs(receiver, nullptr);
        std::vector<llvm::Value*> args{self, selectorGlobal(send.target->selector)};
        for (size_t i = 0; i < arguments.size(); ++i) {
            args.push_back(emitAs(arguments[i], send.target->params[i]->type));
        }
        llvm::FunctionType* signature = methodType(send.target);
        if (send.kind == SendKind::directMethodCall || send.kind == SendKind::superCall) {
            return builder_.CreateCall(functions_.at(send.target), args);
        }
        if (send.kind == SendKind::dynamicSend) {
            return builder_.CreateCall(signature, msgSendFunction_, args);
        }
        llvm::Value* isa = builder_.CreateLoad(builder_.getPtrTy(), self, "isa");
        uint64_t offset = abi::classVtableOffset + static_cast<uint64_t>(send.slot) * 8;
        llvm::Value* entry = builder_.CreateConstInBoundsGEP1_64(builder_.getInt8Ty(), isa, offset);
        llvm::Value* imp = builder_.CreateLoad(builder_.getPtrTy(), entry, "imp");
        return builder_.CreateCall(signature, imp, args);
    }
    case SendKind::fieldGet: {
        auto unary = static_cast<const UnarySend*>(node);
        llvm::Value* value = builder_.CreateLoad(llvmType(send.field->type), fieldAddress(emitAs(unary->receiver, nullptr), send.field));
        if (sema::isReference(send.field->type)) {
            retain(value);
        }
        return value;
    }
    case SendKind::fieldSet: {
        auto keyword = static_cast<const KeywordSend*>(node);
        llvm::Value* object = emitAs(keyword->receiver, nullptr);
        llvm::Value* value = emitOwnedAs(keyword->arguments[0], send.field->type);
        storeReference(fieldAddress(object, send.field), value, send.field->type);
        return nullptr;
    }
    case SendKind::referenceCompare: {
        auto binary = static_cast<const BinarySend*>(node);
        llvm::Value* a = emitAs(binary->receiver, nullptr);
        llvm::Value* b = emitAs(binary->argument, nullptr);
        return send.op == "=" ? builder_.CreateICmpEQ(a, b) : builder_.CreateICmpNE(a, b);
    }
    case SendKind::inlineIfNil: {
        auto keyword = static_cast<const KeywordSend*>(node);
        llvm::Value* value = emitAs(keyword->receiver, nullptr);
        llvm::Value* isNil = builder_.CreateICmpEQ(value, llvm::ConstantPointerNull::get(builder_.getPtrTy()));
        llvm::BasicBlock* nilBlock = llvm::BasicBlock::Create(context_, "ifnil", function_);
        llvm::BasicBlock* valueBlock = llvm::BasicBlock::Create(context_, "ifnotnil", function_);
        llvm::BasicBlock* merge = llvm::BasicBlock::Create(context_, "nilend", function_);
        builder_.CreateCondBr(isNil, nilBlock, valueBlock);
        std::vector<std::string> parts;
        for (size_t start = 0; start < keyword->selector.size();) {
            size_t colon = keyword->selector.find(':', start);
            parts.push_back(keyword->selector.substr(start, colon - start + 1));
            start = colon + 1;
        }
        auto branch = [&](llvm::BasicBlock* start, bool forNil) {
            builder_.SetInsertPoint(start);
            for (size_t i = 0; i < parts.size(); ++i) {
                if ((parts[i] == "ifNil:") != forNil) {
                    continue;
                }
                auto literal = static_cast<const BlockLiteral*>(keyword->arguments[i]);
                if (!forNil) {
                    const sema::LocalInfo* bound = model_.blockOf(literal)->params[0];
                    retain(value);
                    storeReference(addressOf(bound), value, bound->type);
                }
                size_t mark = temporaries_.size();
                emitStatements(literal->statements);
                flushTemporaries(mark);
            }
            if (!builder_.GetInsertBlock()->hasTerminator()) {
                builder_.CreateBr(merge);
            }
        };
        branch(nilBlock, true);
        branch(valueBlock, false);
        builder_.SetInsertPoint(merge);
        return nullptr;
    }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// blocks

// A block value is {function, environment}. The environment is a stack record
// of pointers to the captured variables, valid because blocks don't escape.
llvm::StructType* Generator::blockValueType()
{
    return llvm::StructType::get(context_, {builder_.getPtrTy(), builder_.getPtrTy()});
}

llvm::FunctionType* Generator::blockFunctionType(const Type* blockType)
{
    std::vector<llvm::Type*> params{builder_.getPtrTy()};
    for (const Type* param : blockType->params) {
        params.push_back(llvmType(param));
    }
    return llvm::FunctionType::get(llvmType(blockType->result), params, false);
}

llvm::Value* Generator::emitBlockLiteral(const BlockLiteral* literal)
{
    const sema::BlockInfo* info = model_.blockOf(literal);
    llvm::Function* function = emitBlockFunction(info);

    llvm::Value* environment = llvm::ConstantPointerNull::get(builder_.getPtrTy());
    if (!info->captures.empty()) {
        std::vector<llvm::Type*> fields(info->captures.size(), builder_.getPtrTy());
        llvm::StructType* environmentType = llvm::StructType::get(context_, fields);
        llvm::AllocaInst* record = entryAlloca(environmentType, "env");
        for (size_t i = 0; i < info->captures.size(); ++i) {
            builder_.CreateStore(addressOf(info->captures[i]), builder_.CreateStructGEP(environmentType, record, static_cast<unsigned>(i)));
        }
        environment = record;
    }
    llvm::Value* value = llvm::UndefValue::get(blockValueType());
    value = builder_.CreateInsertValue(value, function, {0});
    return builder_.CreateInsertValue(value, environment, {1});
}

llvm::Function* Generator::emitBlockFunction(const sema::BlockInfo* info)
{
    llvm::Function* savedFunction = function_;
    const sema::BlockInfo* savedBlock = block_;
    llvm::Value* savedEnvironment = environment_;
    auto savedSlots = std::move(slots_);
    auto savedLoops = std::move(loops_);
    auto savedTemporaries = std::move(temporaries_);
    temporaries_.clear();
    llvm::IRBuilderBase::InsertPoint savedPoint = builder_.saveIP();

    const Type* blockType = info->type;
    function_ = llvm::Function::Create(blockFunctionType(blockType), llvm::GlobalValue::InternalLinkage, info->symbolName, module_.get());
    block_ = info;
    environment_ = function_->getArg(0);
    environment_->setName("env");
    slots_.clear();
    loops_.clear();
    builder_.SetInsertPoint(llvm::BasicBlock::Create(context_, "entry", function_));

    for (const sema::LocalInfo* local : info->locals) {
        if (sema::isValueType(local->type)) {
            llvm::AllocaInst* slot = builder_.CreateAlloca(llvmType(local->type), nullptr, local->name);
            slots_[local] = slot;
            if (sema::isReference(local->type) && !local->isParameter) {
                builder_.CreateStore(llvm::ConstantPointerNull::get(builder_.getPtrTy()), slot);
            }
        }
    }
    for (size_t i = 0; i < info->params.size(); ++i) {
        builder_.CreateStore(function_->getArg(static_cast<unsigned>(i + 1)), slots_.at(info->params[i]));
    }

    llvm::Value* result = emitInlinedBody(info->node, blockType->result);
    if (!builder_.GetInsertBlock()->hasTerminator()) {
        emitReturn(blockType->result->kind == TypeKind::voidType ? nullptr : result);
    }

    llvm::Function* function = function_;
    function_ = savedFunction;
    block_ = savedBlock;
    environment_ = savedEnvironment;
    slots_ = std::move(savedSlots);
    loops_ = std::move(savedLoops);
    temporaries_ = std::move(savedTemporaries);
    builder_.restoreIP(savedPoint);
    return function;
}

// Emits a block body in the current function; returns the value of its last
// expression converted to `resultType`, or null when there's no result.
llvm::Value* Generator::emitInlinedBody(const BlockLiteral* literal, const Type* resultType)
{
    const std::vector<Node*>& statements = literal->statements;
    bool wantsValue = resultType != nullptr && resultType->kind != TypeKind::voidType;
    auto last = statements.empty() ? nullptr : nodeCast<ExpressionStmt>(statements.back());
    if (!wantsValue || last == nullptr) {
        emitStatements(statements);
        return nullptr;
    }
    for (size_t i = 0; i + 1 < statements.size(); ++i) {
        emitStatement(statements[i]);
    }
    ensureLiveBlock();
    if (sema::isReference(resultType)) {
        return emitOwnedAs(last->expression, resultType);
    }
    return emitAs(last->expression, resultType);
}

llvm::Value* Generator::emitInvoke(const Node* receiver, const std::vector<Node*>& arguments, const Type* blockType)
{
    llvm::Value* block = emit(receiver);
    llvm::Value* function = builder_.CreateExtractValue(block, {0}, "block.fn");
    std::vector<llvm::Value*> args{builder_.CreateExtractValue(block, {1}, "block.env")};
    for (size_t i = 0; i < arguments.size(); ++i) {
        args.push_back(emitAs(arguments[i], blockType->params[i]));
    }
    return builder_.CreateCall(blockFunctionType(blockType), function, args);
}

llvm::Value* Generator::emitInlinedIf(const KeywordSend* send, const sema::SendInfo& info)
{
    const BlockLiteral* whenTrue = nullptr;
    const BlockLiteral* whenFalse = nullptr;
    const std::string& selector = send->selector;
    if (selector == "ifTrue:" || selector == "ifTrue:ifFalse:") {
        whenTrue = static_cast<const BlockLiteral*>(send->arguments[0]);
        whenFalse = send->arguments.size() > 1 ? static_cast<const BlockLiteral*>(send->arguments[1]) : nullptr;
    }
    else {
        whenFalse = static_cast<const BlockLiteral*>(send->arguments[0]);
        whenTrue = send->arguments.size() > 1 ? static_cast<const BlockLiteral*>(send->arguments[1]) : nullptr;
    }
    const Type* resultType = info.resultType;
    bool wantsValue = resultType->kind != TypeKind::voidType;

    llvm::Value* condition = emitAs(send->receiver, model_.types.boolType());
    llvm::BasicBlock* trueBlock = llvm::BasicBlock::Create(context_, "iftrue", function_);
    llvm::BasicBlock* falseBlock = llvm::BasicBlock::Create(context_, "iffalse", function_);
    llvm::BasicBlock* merge = llvm::BasicBlock::Create(context_, "ifend", function_);
    builder_.CreateCondBr(condition, trueBlock, falseBlock);

    std::vector<std::pair<llvm::Value*, llvm::BasicBlock*>> incoming;
    auto branch = [&](llvm::BasicBlock* start, const BlockLiteral* literal) {
        builder_.SetInsertPoint(start);
        size_t mark = temporaries_.size();
        llvm::Value* value = literal != nullptr ? emitInlinedBody(literal, resultType) : nullptr;
        flushTemporaries(mark);
        if (!builder_.GetInsertBlock()->hasTerminator()) {
            if (wantsValue) {
                incoming.push_back({value, builder_.GetInsertBlock()});
            }
            builder_.CreateBr(merge);
        }
    };
    branch(trueBlock, whenTrue);
    branch(falseBlock, whenFalse);

    builder_.SetInsertPoint(merge);
    if (!wantsValue) {
        return nullptr;
    }
    if (incoming.empty()) {
        return llvm::UndefValue::get(llvmType(resultType));
    }
    llvm::PHINode* phi = builder_.CreatePHI(llvmType(resultType), static_cast<unsigned>(incoming.size()));
    for (const auto& [value, from] : incoming) {
        phi->addIncoming(value, from);
    }
    return phi;
}

void Generator::emitTimesRepeat(const KeywordSend* send, const Type* countType)
{
    auto literal = static_cast<const BlockLiteral*>(send->arguments[0]);
    llvm::Value* count = emitAs(send->receiver, countType);
    llvm::Type* counterType = llvmType(countType);
    llvm::AllocaInst* counter = entryAlloca(counterType, "times");
    builder_.CreateStore(llvm::ConstantInt::get(counterType, 0), counter);

    llvm::BasicBlock* condition = llvm::BasicBlock::Create(context_, "times.cond", function_);
    llvm::BasicBlock* body = llvm::BasicBlock::Create(context_, "times.body", function_);
    llvm::BasicBlock* increment = llvm::BasicBlock::Create(context_, "times.inc", function_);
    llvm::BasicBlock* exit = llvm::BasicBlock::Create(context_, "times.end", function_);
    builder_.CreateBr(condition);

    builder_.SetInsertPoint(condition);
    llvm::Value* done = builder_.CreateLoad(counterType, counter);
    llvm::Value* more = countType->isSigned ? builder_.CreateICmpSLT(done, count) : builder_.CreateICmpULT(done, count);
    builder_.CreateCondBr(more, body, exit);

    builder_.SetInsertPoint(body);
    loops_.push_back({exit, increment});
    emitStatements(literal->statements);
    loops_.pop_back();
    if (!builder_.GetInsertBlock()->hasTerminator()) {
        builder_.CreateBr(increment);
    }

    builder_.SetInsertPoint(increment);
    builder_.CreateStore(builder_.CreateAdd(builder_.CreateLoad(counterType, counter), llvm::ConstantInt::get(counterType, 1)), counter);
    builder_.CreateBr(condition);

    builder_.SetInsertPoint(exit);
}

// ---------------------------------------------------------------------------
// arithmetic

void Generator::trapIf(llvm::Value* condition, abi::TrapReason reason)
{
    llvm::BasicBlock* trap = llvm::BasicBlock::Create(context_, "trap", function_);
    llvm::BasicBlock* continuation = llvm::BasicBlock::Create(context_, "cont", function_);
    builder_.CreateCondBr(condition, trap, continuation);
    builder_.SetInsertPoint(trap);
    llvm::CallInst* call = builder_.CreateCall(trapFunction_, {builder_.getInt32(static_cast<int32_t>(reason))});
    call->setDoesNotReturn();
    builder_.CreateUnreachable();
    builder_.SetInsertPoint(continuation);
}

llvm::Value* Generator::minValue(const Type* type)
{
    unsigned bits = type->bits;
    return llvm::ConstantInt::get(context_, type->isSigned ? llvm::APInt::getSignedMinValue(bits) : llvm::APInt::getMinValue(bits));
}

llvm::Value* Generator::maxValue(const Type* type)
{
    unsigned bits = type->bits;
    return llvm::ConstantInt::get(context_, type->isSigned ? llvm::APInt::getSignedMaxValue(bits) : llvm::APInt::getMaxValue(bits));
}

llvm::Value* Generator::arithmetic(char op, llvm::Value* a, llvm::Value* b, const Type* type, OverflowMode mode)
{
    if (sema::isReal(type)) {
        switch (op) {
        case '+':
            return builder_.CreateFAdd(a, b);
        case '-':
            return builder_.CreateFSub(a, b);
        case '*':
            return builder_.CreateFMul(a, b);
        default:
            return builder_.CreateFDiv(a, b);
        }
    }
    if (op == '/' || op == '%') {
        return divide(op, a, b, type, mode);
    }

    bool isSigned = type->isSigned;
    if (mode == OverflowMode::wrap) {
        return op == '+' ? builder_.CreateAdd(a, b) : op == '-' ? builder_.CreateSub(a, b) : builder_.CreateMul(a, b);
    }
    if (mode == OverflowMode::saturate && op != '*') {
        llvm::Intrinsic::ID id = op == '+' ? (isSigned ? llvm::Intrinsic::sadd_sat : llvm::Intrinsic::uadd_sat)
                                           : (isSigned ? llvm::Intrinsic::ssub_sat : llvm::Intrinsic::usub_sat);
        return builder_.CreateBinaryIntrinsic(id, a, b);
    }

    llvm::Intrinsic::ID id = llvm::Intrinsic::not_intrinsic;
    if (op == '+') {
        id = isSigned ? llvm::Intrinsic::sadd_with_overflow : llvm::Intrinsic::uadd_with_overflow;
    }
    else if (op == '-') {
        id = isSigned ? llvm::Intrinsic::ssub_with_overflow : llvm::Intrinsic::usub_with_overflow;
    }
    else {
        id = isSigned ? llvm::Intrinsic::smul_with_overflow : llvm::Intrinsic::umul_with_overflow;
    }
    llvm::Value* pair = builder_.CreateBinaryIntrinsic(id, a, b);
    llvm::Value* result = builder_.CreateExtractValue(pair, {0});
    llvm::Value* overflowed = builder_.CreateExtractValue(pair, {1});
    if (mode == OverflowMode::trap) {
        trapIf(overflowed, abi::TrapReason::integerOverflow);
        return result;
    }
    llvm::Value* saturated = maxValue(type);
    if (isSigned) {
        llvm::Value* signsDiffer = builder_.CreateICmpSLT(builder_.CreateXor(a, b), llvm::ConstantInt::get(a->getType(), 0));
        saturated = builder_.CreateSelect(signsDiffer, minValue(type), maxValue(type));
    }
    return builder_.CreateSelect(overflowed, saturated, result);
}

llvm::Value* Generator::divide(char op, llvm::Value* a, llvm::Value* b, const Type* type, OverflowMode mode)
{
    llvm::Value* zero = llvm::ConstantInt::get(b->getType(), 0);
    trapIf(builder_.CreateICmpEQ(b, zero), abi::TrapReason::divideByZero);
    if (!type->isSigned) {
        return op == '/' ? builder_.CreateUDiv(a, b) : builder_.CreateURem(a, b);
    }
    llvm::Value* special = builder_.CreateAnd(builder_.CreateICmpEQ(a, minValue(type)),
                                              builder_.CreateICmpEQ(b, llvm::ConstantInt::getSigned(b->getType(), -1)));
    if (mode == OverflowMode::trap && op == '/') {
        trapIf(special, abi::TrapReason::integerOverflow);
        return builder_.CreateSDiv(a, b);
    }
    llvm::Value* safeDivisor = builder_.CreateSelect(special, llvm::ConstantInt::get(b->getType(), 1), b);
    if (op == '%') {
        return builder_.CreateSelect(special, zero, builder_.CreateSRem(a, safeDivisor));
    }
    llvm::Value* quotient = builder_.CreateSDiv(a, safeDivisor);
    return builder_.CreateSelect(special, mode == OverflowMode::wrap ? minValue(type) : maxValue(type), quotient);
}

llvm::Value* Generator::negate(llvm::Value* value, const Type* type, OverflowMode mode)
{
    if (sema::isReal(type)) {
        return builder_.CreateFNeg(value);
    }
    return arithmetic('-', llvm::ConstantInt::get(value->getType(), 0), value, type, mode);
}

llvm::Value* Generator::compare(const std::string& op, llvm::Value* a, llvm::Value* b, const Type* type)
{
    if (sema::isReal(type)) {
        if (op == "=") {
            return builder_.CreateFCmpOEQ(a, b);
        }
        if (op == "<>") {
            return builder_.CreateFCmpUNE(a, b);
        }
        if (op == "<") {
            return builder_.CreateFCmpOLT(a, b);
        }
        if (op == "<=") {
            return builder_.CreateFCmpOLE(a, b);
        }
        if (op == ">") {
            return builder_.CreateFCmpOGT(a, b);
        }
        return builder_.CreateFCmpOGE(a, b);
    }
    bool isSigned = type->isSigned;
    if (op == "=") {
        return builder_.CreateICmpEQ(a, b);
    }
    if (op == "<>") {
        return builder_.CreateICmpNE(a, b);
    }
    if (op == "<") {
        return isSigned ? builder_.CreateICmpSLT(a, b) : builder_.CreateICmpULT(a, b);
    }
    if (op == "<=") {
        return isSigned ? builder_.CreateICmpSLE(a, b) : builder_.CreateICmpULE(a, b);
    }
    if (op == ">") {
        return isSigned ? builder_.CreateICmpSGT(a, b) : builder_.CreateICmpUGT(a, b);
    }
    return isSigned ? builder_.CreateICmpSGE(a, b) : builder_.CreateICmpUGE(a, b);
}

// Explicit `asT` conversions. Narrowing or sign-changing integer conversions and
// real-to-integer conversions are checked and trap when the value doesn't fit.
llvm::Value* Generator::convert(llvm::Value* value, const Type* from, const Type* to)
{
    if (from == to) {
        return value;
    }
    llvm::Type* target = llvmType(to);
    if (sema::isInteger(from) && sema::isInteger(to)) {
        if (sema::canWiden(from, to)) {
            return widen(value, from, to);
        }
        llvm::Type* wide = builder_.getIntNTy(128);
        llvm::Value* extended = from->isSigned ? builder_.CreateSExt(value, wide) : builder_.CreateZExt(value, wide);
        llvm::APInt low = to->isSigned ? llvm::APInt::getSignedMinValue(to->bits).sext(128) : llvm::APInt(128, 0);
        llvm::APInt high = to->isSigned ? llvm::APInt::getSignedMaxValue(to->bits).sext(128) : llvm::APInt::getMaxValue(to->bits).zext(128);
        llvm::Value* inRange = builder_.CreateAnd(builder_.CreateICmpSGE(extended, llvm::ConstantInt::get(context_, low)),
                                                  builder_.CreateICmpSLE(extended, llvm::ConstantInt::get(context_, high)));
        trapIf(builder_.CreateNot(inRange), abi::TrapReason::conversionOutOfRange);
        if (to->bits < from->bits) {
            return builder_.CreateTrunc(value, target);
        }
        if (to->bits > from->bits) {
            return from->isSigned ? builder_.CreateSExt(value, target) : builder_.CreateZExt(value, target);
        }
        return value;
    }
    if (sema::isInteger(from) && sema::isReal(to)) {
        return from->isSigned ? builder_.CreateSIToFP(value, target) : builder_.CreateUIToFP(value, target);
    }
    if (sema::isReal(from) && sema::isReal(to)) {
        return to->bits > from->bits ? builder_.CreateFPExt(value, target) : builder_.CreateFPTrunc(value, target);
    }
    llvm::Value* real = from->bits == 32 ? builder_.CreateFPExt(value, builder_.getDoubleTy()) : value;
    llvm::Type* doubleType = builder_.getDoubleTy();
    llvm::Value* aboveLow = nullptr;
    double upper = std::ldexp(1.0, static_cast<int>(to->isSigned ? to->bits - 1 : to->bits));
    if (to->isSigned) {
        double low = -std::ldexp(1.0, static_cast<int>(to->bits - 1));
        aboveLow = to->bits < 64 ? builder_.CreateFCmpOGT(real, llvm::ConstantFP::get(doubleType, low - 1.0))
                                 : builder_.CreateFCmpOGE(real, llvm::ConstantFP::get(doubleType, low));
    }
    else {
        aboveLow = builder_.CreateFCmpOGT(real, llvm::ConstantFP::get(doubleType, -1.0));
    }
    llvm::Value* belowHigh = builder_.CreateFCmpOLT(real, llvm::ConstantFP::get(doubleType, upper));
    trapIf(builder_.CreateNot(builder_.CreateAnd(aboveLow, belowHigh)), abi::TrapReason::conversionOutOfRange);
    return to->isSigned ? builder_.CreateFPToSI(real, target) : builder_.CreateFPToUI(real, target);
}

llvm::Value* Generator::shift(bool left, llvm::Value* value, llvm::Value* amount, const Type* type, const Type* amountType,
                              OverflowMode mode)
{
    llvm::Value* tooFar = amountType->isSigned
                              ? builder_.CreateOr(builder_.CreateICmpSLT(amount, llvm::ConstantInt::get(amount->getType(), 0)),
                                                  builder_.CreateICmpSGE(amount, llvm::ConstantInt::get(amount->getType(), type->bits)))
                              : builder_.CreateICmpUGE(amount, llvm::ConstantInt::get(amount->getType(), type->bits));
    trapIf(tooFar, abi::TrapReason::shiftOutOfRange);
    llvm::Value* count = builder_.CreateZExtOrTrunc(amount, value->getType());
    if (!left) {
        return type->isSigned ? builder_.CreateAShr(value, count) : builder_.CreateLShr(value, count);
    }
    llvm::Value* shifted = builder_.CreateShl(value, count);
    if (mode == OverflowMode::wrap) {
        return shifted;
    }
    llvm::Value* back = type->isSigned ? builder_.CreateAShr(shifted, count) : builder_.CreateLShr(shifted, count);
    llvm::Value* lost = builder_.CreateICmpNE(back, value);
    if (mode == OverflowMode::trap) {
        trapIf(lost, abi::TrapReason::integerOverflow);
        return shifted;
    }
    llvm::Value* saturated = maxValue(type);
    if (type->isSigned) {
        saturated = builder_.CreateSelect(builder_.CreateICmpSLT(value, llvm::ConstantInt::get(value->getType(), 0)), minValue(type), maxValue(type));
    }
    return builder_.CreateSelect(lost, saturated, shifted);
}

}

std::unique_ptr<llvm::Module> generateModule(const ast::Unit* unit, const sema::SemanticModel& model, llvm::LLVMContext& context,
                                             const std::string& dataLayout)
{
    Generator generator(model, context, dataLayout);
    return generator.run(unit);
}

}
