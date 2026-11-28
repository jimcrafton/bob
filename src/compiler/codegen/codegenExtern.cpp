// C functions (design §6.6): imports, exports, C function pointers made from
// capture-free blocks, and calls through them, on the Windows x64 C ABI.
// bob's own functions pass records as LLVM aggregates; at the boundary a
// record of 1, 2, 4 or 8 bytes travels as an integer of that size and any
// other record by reference to a copy (a result through a hidden sret pointer).

#include "codegenInternal.h"

#include <algorithm>

namespace bob::codegen::detail {

CPassing Generator::cPassing(const Type* type)
{
    type = sema::canonical(type);
    if (type->kind == TypeKind::record) {
        uint64_t size = module_->getDataLayout().getTypeAllocSize(structType(type->record));
        if (size == 1 || size == 2 || size == 4 || size == 8) {
            return {CPassing::Kind::asInteger, builder_.getIntNTy(static_cast<unsigned>(size * 8))};
        }
        return {CPassing::Kind::indirect, builder_.getPtrTy()};
    }
    return {CPassing::Kind::direct, llvmType(type)};
}

CSignature Generator::cSignature(const std::vector<const Type*>& params, const Type* result)
{
    CSignature signature;
    signature.paramTypes = params;
    signature.resultType = result;
    std::vector<llvm::Type*> types;
    llvm::Type* returnType = builder_.getVoidTy();
    if (result->kind != TypeKind::voidType) {
        signature.result = cPassing(result);
        if (signature.result.kind == CPassing::Kind::indirect) {
            signature.sret = true;
            types.push_back(builder_.getPtrTy());
        }
        else {
            returnType = signature.result.type;
        }
    }
    for (const Type* param : params) {
        signature.params.push_back(cPassing(param));
        types.push_back(signature.params.back().type);
    }
    signature.type = llvm::FunctionType::get(returnType, types, false);
    return signature;
}

namespace {

// C's _Bool is one byte holding 0 or 1; LLVM's i1 crosses with zeroext.
template <class Target>
void addCAttributes(Target* target, const CSignature& signature, llvm::LLVMContext& context, llvm::Type* resultType)
{
    unsigned offset = signature.sret ? 1 : 0;
    if (signature.sret) {
        target->addParamAttr(0, llvm::Attribute::getWithStructRetType(context, resultType));
        target->addParamAttr(0, llvm::Attribute::NoAlias);
    }
    else if (signature.resultType->kind == TypeKind::boolType) {
        target->addRetAttr(llvm::Attribute::ZExt);
    }
    for (size_t i = 0; i < signature.paramTypes.size(); ++i) {
        if (sema::canonical(signature.paramTypes[i])->kind == TypeKind::boolType) {
            target->addParamAttr(static_cast<unsigned>(i) + offset, llvm::Attribute::ZExt);
        }
    }
}

}

void Generator::applyCAttributes(llvm::Function* function, const CSignature& signature)
{
    addCAttributes(function, signature, context_, signature.sret ? llvmType(signature.resultType) : nullptr);
}

void Generator::applyCAttributes(llvm::CallInst* call, const CSignature& signature)
{
    addCAttributes(call, signature, context_, signature.sret ? llvmType(signature.resultType) : nullptr);
}

llvm::Value* Generator::toC(llvm::Value* value, const Type* type, const CPassing& passing)
{
    if (passing.kind == CPassing::Kind::direct) {
        return value;
    }
    llvm::AllocaInst* slot = entryAlloca(llvmType(type), "c.arg");
    slot->setAlignment(llvm::Align(std::max<uint64_t>(slot->getAlign().value(), 8)));
    builder_.CreateStore(value, slot);
    if (passing.kind == CPassing::Kind::asInteger) {
        return builder_.CreateLoad(passing.type, slot, "c.bits");
    }
    return slot;
}

llvm::Value* Generator::fromC(llvm::Value* value, const Type* type, const CPassing& passing)
{
    switch (passing.kind) {
    case CPassing::Kind::direct:
        return value;
    case CPassing::Kind::asInteger: {
        llvm::AllocaInst* slot = entryAlloca(llvmType(type), "c.value");
        slot->setAlignment(llvm::Align(std::max<uint64_t>(slot->getAlign().value(), 8)));
        builder_.CreateStore(value, slot);
        return builder_.CreateLoad(llvmType(type), slot);
    }
    case CPassing::Kind::indirect:
        return builder_.CreateLoad(llvmType(type), value);
    }
    return value;
}

llvm::Value* Generator::emitCCall(llvm::Value* callee, const CSignature& signature, const std::vector<llvm::Value*>& args)
{
    std::vector<llvm::Value*> cArgs;
    llvm::AllocaInst* result = nullptr;
    if (signature.sret) {
        result = entryAlloca(llvmType(signature.resultType), "c.result");
        cArgs.push_back(result);
    }
    for (size_t i = 0; i < args.size(); ++i) {
        cArgs.push_back(toC(args[i], signature.paramTypes[i], signature.params[i]));
    }
    llvm::CallInst* call = builder_.CreateCall(signature.type, callee, cArgs);
    applyCAttributes(call, signature);
    if (signature.sret) {
        return builder_.CreateLoad(llvmType(signature.resultType), result);
    }
    if (signature.resultType->kind == TypeKind::voidType) {
        return nullptr;
    }
    return fromC(call, signature.resultType, signature.result);
}

llvm::Function* Generator::cDefinition(const std::string& name, llvm::FunctionType* type, llvm::GlobalValue::LinkageTypes linkage)
{
    llvm::Function* function = llvm::Function::Create(type, linkage, "", module_.get());
    if (llvm::Function* existing = module_->getFunction(name); existing != nullptr && existing->isDeclaration()) {
        existing->replaceAllUsesWith(function);
        existing->eraseFromParent();
    }
    function->setName(name);
    return function;
}

llvm::Function* Generator::cDeclaration(const std::string& name, const CSignature& signature)
{
    if (llvm::Function* existing = module_->getFunction(name)) {
        return existing;
    }
    llvm::Function* function = llvm::Function::Create(signature.type, llvm::GlobalValue::ExternalLinkage, name, module_.get());
    applyCAttributes(function, signature);
    return function;
}

llvm::Function* Generator::emitCThunk(const std::string& name, llvm::GlobalValue::LinkageTypes linkage, const CSignature& signature,
                                      llvm::Function* target, bool withEnvironment)
{
    llvm::Function* thunk = cDefinition(name, signature.type, linkage);
    applyCAttributes(thunk, signature);
    llvm::Function* savedFunction = function_;
    llvm::IRBuilderBase::InsertPoint savedPoint = builder_.saveIP();
    function_ = thunk;
    builder_.SetInsertPoint(llvm::BasicBlock::Create(context_, "entry", thunk));

    unsigned offset = signature.sret ? 1 : 0;
    std::vector<llvm::Value*> args;
    if (withEnvironment) {
        args.push_back(llvm::ConstantPointerNull::get(builder_.getPtrTy()));
    }
    for (size_t i = 0; i < signature.paramTypes.size(); ++i) {
        args.push_back(fromC(thunk->getArg(static_cast<unsigned>(i) + offset), signature.paramTypes[i], signature.params[i]));
    }
    llvm::CallInst* value = builder_.CreateCall(target, args);
    if (signature.sret) {
        builder_.CreateStore(value, thunk->getArg(0));
        builder_.CreateRetVoid();
    }
    else if (signature.resultType->kind == TypeKind::voidType) {
        builder_.CreateRetVoid();
    }
    else {
        builder_.CreateRet(toC(value, signature.resultType, signature.result));
    }

    function_ = savedFunction;
    builder_.restoreIP(savedPoint);
    return thunk;
}

namespace {

std::vector<const Type*> paramTypesOf(const sema::MessageInfo* message)
{
    std::vector<const Type*> types;
    for (const sema::LocalInfo* param : message->params) {
        types.push_back(param->type);
    }
    return types;
}

}

// The message's own function becomes a small inline wrapper that calls the C
// function, so every bob call site stays an ordinary static call.
void Generator::emitExternImport(const sema::MessageInfo* message)
{
    llvm::Function* wrapper = functions_.at(message);
    wrapper->addFnAttr(llvm::Attribute::AlwaysInline);
    function_ = wrapper;
    builder_.SetInsertPoint(llvm::BasicBlock::Create(context_, "entry", wrapper));
    CSignature signature = cSignature(paramTypesOf(message), message->returnType);
    std::vector<llvm::Value*> args;
    for (llvm::Argument& argument : wrapper->args()) {
        args.push_back(&argument);
    }
    llvm::Value* result = emitCCall(cDeclaration(message->cSymbol, signature), signature, args);
    if (result != nullptr) {
        builder_.CreateRet(result);
    }
    else {
        builder_.CreateRetVoid();
    }
}

void Generator::emitExport(const sema::MessageInfo* message)
{
    CSignature signature = cSignature(paramTypesOf(message), message->returnType);
    emitCThunk(message->cSymbol, llvm::GlobalValue::ExternalLinkage, signature, functions_.at(message), false);
}

llvm::Value* Generator::emitCFunctionBlock(const sema::BlockInfo* info)
{
    llvm::Function* body = emitBlockFunction(info);
    CSignature signature = cSignature(info->type->params, info->type->result);
    return emitCThunk(info->symbolName + ".c", llvm::GlobalValue::InternalLinkage, signature, body, true);
}

llvm::Value* Generator::emitCFunctionInvoke(const Node* node, const sema::SendInfo& send)
{
    const Node* receiver = nullptr;
    std::vector<Node*> arguments;
    if (auto unary = nodeCast<UnarySend>(node)) {
        receiver = unary->receiver;
    }
    else {
        auto keyword = static_cast<const KeywordSend*>(node);
        receiver = keyword->receiver;
        arguments = keyword->arguments;
    }
    const Type* type = sema::canonical(send.operandType);
    llvm::Value* callee = emitAs(receiver, nullptr);
    std::vector<llvm::Value*> args;
    for (size_t i = 0; i < arguments.size(); ++i) {
        args.push_back(emitAs(arguments[i], type->params[i]));
    }
    return emitCCall(callee, cSignature(type->params, type->result), args);
}

}
