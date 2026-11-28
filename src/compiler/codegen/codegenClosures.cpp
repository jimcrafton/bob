// Block values, closure environments and boxed variables (design §5.5).

#include "codegenInternal.h"

#include <algorithm>

namespace bob::codegen::detail {

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

bool Generator::isHeapBlock(const sema::BlockInfo* info) const
{
    return !info->inlined && model_.stackBlocks.count(info) == 0;
}

bool Generator::capturedByValue(const sema::BlockInfo* info, const sema::LocalInfo* local) const
{
    return isHeapBlock(info) && local->readOnly;
}

bool Generator::isBoxed(const sema::LocalInfo* local) const
{
    return model_.boxedLocals.count(local) != 0;
}

// { class*, refCount, flags, captures... }
llvm::StructType* Generator::environmentType(const sema::BlockInfo* info)
{
    auto it = environmentTypes_.find(info);
    if (it != environmentTypes_.end()) {
        return it->second;
    }
    std::vector<llvm::Type*> fields{builder_.getPtrTy(), builder_.getInt32Ty(), builder_.getInt32Ty()};
    for (const sema::LocalInfo* local : info->captures) {
        fields.push_back(capturedByValue(info, local) ? llvmType(local->type) : builder_.getPtrTy());
    }
    llvm::StructType* type = llvm::StructType::get(context_, fields);
    environmentTypes_[info] = type;
    return type;
}

llvm::GlobalVariable* Generator::plainClass(const std::string& name, llvm::StructType* layout, llvm::Function* destroy)
{
    llvm::Type* ptr = builder_.getPtrTy();
    llvm::StructType* type = classInfoType(0);
    llvm::Constant* text = stringLiteral(name);
    llvm::Constant* null = llvm::ConstantPointerNull::get(ptr);
    uint64_t size = module_->getDataLayout().getTypeAllocSize(layout);
    llvm::Constant* initializer = llvm::ConstantStruct::get(
        type, {null, text, builder_.getInt64(size), destroy != nullptr ? static_cast<llvm::Constant*>(destroy) : null, null, builder_.getInt32(0),
               builder_.getInt32(0), null, null, null, llvm::ConstantArray::get(llvm::ArrayType::get(ptr, 0), {})});
    return new llvm::GlobalVariable(*module_, type, false, llvm::GlobalValue::InternalLinkage, initializer, "class." + name);
}

llvm::Value* Generator::boxOf(llvm::Value* address)
{
    llvm::Value* offset = llvm::ConstantInt::getSigned(builder_.getInt64Ty(), -static_cast<int64_t>(abi::objectHeaderSize));
    return builder_.CreateInBoundsGEP(builder_.getInt8Ty(), address, offset, "box");
}

// The class of a heap closure: its destroy releases what the closure holds.
llvm::GlobalVariable* Generator::environmentClass(const sema::BlockInfo* info)
{
    auto it = environmentClasses_.find(info);
    if (it != environmentClasses_.end()) {
        return it->second;
    }
    llvm::StructType* type = environmentType(info);
    auto* destroy = llvm::Function::Create(llvm::FunctionType::get(builder_.getVoidTy(), {builder_.getPtrTy()}, false),
                                           llvm::GlobalValue::InternalLinkage, info->symbolName + ".destroy", module_.get());
    llvm::Function* savedFunction = function_;
    llvm::IRBuilderBase::InsertPoint savedPoint = builder_.saveIP();
    function_ = destroy;
    builder_.SetInsertPoint(llvm::BasicBlock::Create(context_, "entry", destroy));
    for (size_t i = 0; i < info->captures.size(); ++i) {
        const sema::LocalInfo* local = info->captures[i];
        llvm::Value* slot = builder_.CreateStructGEP(type, destroy->getArg(0), static_cast<unsigned>(i + 3));
        if (capturedByValue(info, local)) {
            releaseValue(builder_.CreateLoad(llvmType(local->type), slot), local->type);
        }
        else {
            builder_.CreateCall(releaseFunction_, {boxOf(builder_.CreateLoad(builder_.getPtrTy(), slot))});
        }
    }
    builder_.CreateRetVoid();
    function_ = savedFunction;
    builder_.restoreIP(savedPoint);

    llvm::GlobalVariable* global = plainClass(info->symbolName + ".closure", type, destroy);
    environmentClasses_[info] = global;
    return global;
}

// A box is an object whose one field is the variable.
llvm::GlobalVariable* Generator::boxClass(const sema::LocalInfo* local)
{
    auto it = boxClasses_.find(local);
    if (it != boxClasses_.end()) {
        return it->second;
    }
    llvm::StructType* layout =
        llvm::StructType::get(context_, {builder_.getPtrTy(), builder_.getInt32Ty(), builder_.getInt32Ty(), llvmType(local->type)});
    std::string name = "box." + std::to_string(boxClasses_.size()) + "." + local->name;
    llvm::Function* destroy = nullptr;
    if (sema::needsCleanup(local->type)) {
        destroy = llvm::Function::Create(llvm::FunctionType::get(builder_.getVoidTy(), {builder_.getPtrTy()}, false),
                                         llvm::GlobalValue::InternalLinkage, "bob." + name + ".destroy", module_.get());
        llvm::Function* savedFunction = function_;
        llvm::IRBuilderBase::InsertPoint savedPoint = builder_.saveIP();
        function_ = destroy;
        builder_.SetInsertPoint(llvm::BasicBlock::Create(context_, "entry", destroy));
        llvm::Value* value = builder_.CreateLoad(llvmType(local->type), builder_.CreateStructGEP(layout, destroy->getArg(0), 3));
        dropHeld(value, local->type, local->ownership);
        builder_.CreateRetVoid();
        function_ = savedFunction;
        builder_.restoreIP(savedPoint);
    }
    llvm::GlobalVariable* global = plainClass(name, layout, destroy);
    boxClasses_[local] = global;
    return global;
}

void Generator::allocateBox(const sema::LocalInfo* local)
{
    llvm::AllocaInst* slot = slots_.at(local);
    llvm::Value* old = builder_.CreateLoad(builder_.getPtrTy(), slot, local->name + ".oldbox");
    builder_.CreateStore(builder_.CreateCall(allocFunction_, {boxClass(local)}, local->name + ".box"), slot);
    builder_.CreateCall(releaseFunction_, {old});
}

void Generator::createSlots(const std::vector<sema::LocalInfo*>& locals)
{
    for (const sema::LocalInfo* local : locals) {
        if (!sema::isValueType(local->type)) {
            continue;
        }
        if (isBoxed(local)) {
            llvm::AllocaInst* slot = builder_.CreateAlloca(builder_.getPtrTy(), nullptr, local->name + ".slot");
            slots_[local] = slot;
            builder_.CreateStore(builder_.CreateCall(allocFunction_, {boxClass(local)}, local->name + ".box"), slot);
            continue;
        }
        llvm::AllocaInst* slot = builder_.CreateAlloca(llvmType(local->type), nullptr, local->name);
        if (local->align > slot->getAlign().value()) {
            slot->setAlignment(llvm::Align(local->align));
        }
        slots_[local] = slot;
        if (sema::needsCleanup(local->type) && !local->isParameter) {
            builder_.CreateStore(llvm::Constant::getNullValue(llvmType(local->type)), slot);
        }
    }
}

// ---------------------------------------------------------------------------
// weak and unowned references

llvm::Value* Generator::loadHeld(llvm::Value* address, const Type* type, sema::Ownership ownership, const std::string& name)
{
    llvm::Value* value = builder_.CreateLoad(llvmType(type), address, name);
    if (ownership == sema::Ownership::strong) {
        return value;
    }
    const char* load = ownership == sema::Ownership::weak ? "bobWeakLoad" : "bobUnownedLoad";
    return builder_.CreateCall(runtimeFunction(load, builder_.getPtrTy(), {builder_.getPtrTy()}), {value}, name + ".strong");
}

void Generator::storeHeld(llvm::Value* address, llvm::Value* owned, const Type* type, sema::Ownership ownership)
{
    if (ownership == sema::Ownership::strong) {
        storeReference(address, owned, type);
        return;
    }
    llvm::Value* old = builder_.CreateLoad(builder_.getPtrTy(), address, "old");
    initializeHeld(address, owned, type, ownership);
    dropHeld(old, type, ownership);
}

void Generator::initializeHeld(llvm::Value* address, llvm::Value* owned, const Type* type, sema::Ownership ownership)
{
    if (ownership == sema::Ownership::strong) {
        builder_.CreateStore(owned, address);
        return;
    }
    copyHeld(owned, type, ownership);
    builder_.CreateStore(owned, address);
    releaseValue(owned, type);
}

void Generator::dropHeld(llvm::Value* value, const Type* type, sema::Ownership ownership)
{
    if (ownership == sema::Ownership::strong) {
        releaseValue(value, type);
        return;
    }
    builder_.CreateCall(runtimeFunction("bobWeakRelease", builder_.getVoidTy(), {builder_.getPtrTy()}), {value});
}

void Generator::copyHeld(llvm::Value* value, const Type* type, sema::Ownership ownership)
{
    if (ownership == sema::Ownership::strong) {
        retainValue(value, type);
        return;
    }
    builder_.CreateCall(runtimeFunction("bobWeakRetain", builder_.getVoidTy(), {builder_.getPtrTy()}), {value});
}

const sema::FieldInfo* Generator::assignedField(const Node* target) const
{
    auto self = model_.selfFieldRefs.find(target->id);
    if (self != model_.selfFieldRefs.end()) {
        return self->second;
    }
    if (nodeCast<FieldAccess>(target) != nullptr) {
        return model_.fieldOf(target);
    }
    return nullptr;
}

llvm::Value* Generator::emitBlockLiteral(const BlockLiteral* literal)
{
    const sema::BlockInfo* info = model_.blockOf(literal);
    if (model_.cFunctionBlocks.count(info) != 0) {
        return emitCFunctionBlock(info);
    }
    llvm::Function* function = emitBlockFunction(info);

    llvm::Value* environment = llvm::ConstantPointerNull::get(builder_.getPtrTy());
    if (!info->captures.empty()) {
        llvm::StructType* type = environmentType(info);
        if (isHeapBlock(info)) {
            environment = builder_.CreateCall(allocFunction_, {environmentClass(info)}, "closure");
        }
        else {
            llvm::AllocaInst* record = entryAlloca(type, "env");
            record->setAlignment(llvm::Align(16));
            builder_.CreateStore(llvm::ConstantPointerNull::get(builder_.getPtrTy()), builder_.CreateStructGEP(type, record, 0));
            builder_.CreateStore(builder_.getInt32(1), builder_.CreateStructGEP(type, record, 1));
            builder_.CreateStore(builder_.getInt32(bobObjectImmortal | bobObjectStack), builder_.CreateStructGEP(type, record, 2));
            environment = record;
        }
        for (size_t i = 0; i < info->captures.size(); ++i) {
            const sema::LocalInfo* local = info->captures[i];
            llvm::Value* address = addressOf(local);
            llvm::Value* slot = builder_.CreateStructGEP(type, environment, static_cast<unsigned>(i + 3));
            if (capturedByValue(info, local)) {
                llvm::Value* value = builder_.CreateLoad(llvmType(local->type), address, local->name);
                retainValue(value, local->type);
                builder_.CreateStore(value, slot);
                continue;
            }
            if (isHeapBlock(info)) {
                builder_.CreateCall(retainFunction_, {boxOf(address)});
            }
            builder_.CreateStore(address, slot);
        }
    }
    llvm::Value* value = llvm::UndefValue::get(blockValueType());
    value = builder_.CreateInsertValue(value, function, {0});
    return builder_.CreateInsertValue(value, environment, {1});
}

}
