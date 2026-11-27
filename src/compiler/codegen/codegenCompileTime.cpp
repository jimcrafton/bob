// Compile time (design §8): the evaluation thunk, embedding compile-time
// values, `meta fields` and `compiler error:`.

#include "codegenInternal.h"

#include "layout.h"

#include <llvm/IR/DataLayout.h>

namespace bob::codegen::detail {

// The synthesized messages compile-time items are checked in; they have no
// declaration of their own and only exist inside an evaluation thunk.
bool Generator::isCompileTimeContext(const sema::MessageInfo* message) const
{
    return message->compileTimeOnly && message->decl == nullptr && message->owner == nullptr;
}

uint64_t Generator::measure(const Type* type)
{
    module_ = std::make_unique<llvm::Module>("measure", context_);
    module_->setDataLayout(dataLayout_);
    uint64_t size = module_->getDataLayout().getTypeAllocSize(llvmType(type));
    module_.reset();
    return size;
}

// `void thunk(ptr out)`: runs the item; an expression or constant stores its
// value (owned) at `out`.
void Generator::emitThunk()
{
    const sema::CompileTimeItem* item = evaluating_;
    auto* function = llvm::Function::Create(llvm::FunctionType::get(builder_.getVoidTy(), {builder_.getPtrTy()}, false),
                                            llvm::GlobalValue::ExternalLinkage, thunkName_, module_.get());
    beginFunction(function, item->context, item->context->locals);
    if (item->kind == sema::CompileTimeItem::Kind::block) {
        emitStatements(static_cast<const StatementGroup*>(item->code)->statements);
    }
    else {
        size_t mark = temporaries_.size();
        llvm::Value* value = emitOwnedAs(item->code, item->type);
        builder_.CreateStore(value, function->getArg(0));
        flushTemporaries(mark);
    }
    if (!blockTerminated()) {
        emitReturn(nullptr);
    }
}

llvm::Constant* Generator::constantOf(const sema::ConstValue& value, const Type* type)
{
    type = sema::canonical(type);
    llvm::Type* llvm = llvmType(type);
    switch (type->kind) {
    case TypeKind::boolType:
        return builder_.getInt1((value.bits & 1) != 0);
    case TypeKind::integer:
    case TypeKind::enumType:
    case TypeKind::subrange:
    case TypeKind::character:
        return llvm::ConstantInt::get(llvm, value.bits);
    case TypeKind::set:
        return sema::isBitSet(type) ? llvm::ConstantInt::get(llvm, value.bits) : nullptr;
    case TypeKind::real:
        return llvm::ConstantFP::get(llvm, value.real);
    case TypeKind::string:
        return stringLiteral(value.text);
    case TypeKind::record: {
        std::vector<llvm::Constant*> fields;
        for (size_t i = 0; i < type->record->fields.size(); ++i) {
            llvm::Constant* field = constantOf(value.elements[i], type->record->fields[i].type);
            if (field == nullptr) {
                return nullptr;
            }
            fields.push_back(field);
        }
        return llvm::ConstantStruct::get(llvm::cast<llvm::StructType>(llvm), fields);
    }
    case TypeKind::fixedArray: {
        std::vector<llvm::Constant*> elements;
        for (const sema::ConstValue& element : value.elements) {
            llvm::Constant* constant = constantOf(element, type->inner);
            if (constant == nullptr) {
                return nullptr;
            }
            elements.push_back(constant);
        }
        return llvm::ConstantArray::get(llvm::cast<llvm::ArrayType>(llvm), elements);
    }
    case TypeKind::optional: {
        if (sema::isReference(type)) {
            return nullptr;
        }
        if (!value.present) {
            return defaultValue(type);
        }
        llvm::Constant* inner = constantOf(value.elements[0], type->inner);
        if (inner == nullptr) {
            return nullptr;
        }
        return llvm::ConstantStruct::get(llvm::cast<llvm::StructType>(llvm), {builder_.getTrue(), inner});
    }
    case TypeKind::array: {
        if (value.elements.empty()) {
            return llvm::ConstantPointerNull::get(builder_.getPtrTy());
        }
        if (auto cached = constantArrays_.find(&value); cached != constantArrays_.end()) {
            return cached->second;
        }
        // An immortal array in read-only data: { class, refCount, flags, count, capacity, element, elements... }.
        std::vector<llvm::Constant*> elements;
        for (const sema::ConstValue& element : value.elements) {
            llvm::Constant* constant = constantOf(element, type->inner);
            if (constant == nullptr) {
                return nullptr;
            }
            elements.push_back(constant);
        }
        llvm::ArrayType* dataType = llvm::ArrayType::get(llvmType(type->inner), elements.size());
        llvm::StructType* layout = llvm::StructType::get(context_, {builder_.getPtrTy(), builder_.getInt32Ty(), builder_.getInt32Ty(),
                                                                    builder_.getInt64Ty(), builder_.getInt64Ty(), builder_.getPtrTy(), dataType});
        llvm::Constant* cls = module_->getOrInsertGlobal("bobArrayClass", builder_.getInt8Ty());
        uint64_t count = elements.size();
        llvm::Constant* initial = llvm::ConstantStruct::get(
            layout, {cls, builder_.getInt32(1), builder_.getInt32(bobObjectImmortal), builder_.getInt64(count), builder_.getInt64(count),
                     elementDescriptor(type->inner), llvm::ConstantArray::get(dataType, elements)});
        auto global = new llvm::GlobalVariable(*module_, layout, true, llvm::GlobalValue::PrivateLinkage, initial, "bob.constant.array");
        global->setAlignment(llvm::Align(16));
        constantArrays_[&value] = global;
        return global;
    }
    default:
        return nullptr;
    }
}

llvm::Value* Generator::materialize(const sema::ConstValue* value, const Type* type)
{
    type = sema::canonical(type);
    if (value == nullptr) {
        return defaultValue(type); // a constant no evaluated code depends on
    }
    if (llvm::Constant* constant = constantOf(*value, type)) {
        return constant;
    }
    llvm::Type* ptr = builder_.getPtrTy();
    llvm::Constant* none = llvm::ConstantPointerNull::get(builder_.getPtrTy());
    switch (type->kind) {
    case TypeKind::optional:
        return makeOptional(builder_.getTrue(), materialize(&value->elements[0], type->inner), type);
    case TypeKind::fixedArray: {
        llvm::Value* result = llvm::UndefValue::get(llvmType(type));
        for (size_t i = 0; i < value->elements.size(); ++i) {
            result = builder_.CreateInsertValue(result, materialize(&value->elements[i], type->inner), {static_cast<unsigned>(i)});
        }
        return result;
    }
    case TypeKind::array:
    case TypeKind::set:
    case TypeKind::dictionary:
    case TypeKind::hashtable: {
        // Built at run time; the result is owned.
        llvm::AllocaInst* slot = entryAlloca(ptr, "constant");
        builder_.CreateStore(none, slot);
        if (type->kind == TypeKind::array) {
            for (const sema::ConstValue& element : value->elements) {
                llvm::Value* owned = spill(materialize(&element, type->inner), "element");
                builder_.CreateCall(runtimeFunction("bobArrayPush", builder_.getVoidTy(), {ptr, ptr, ptr}), {slot, owned, elementDescriptor(type->inner)});
            }
        }
        else if (type->kind == TypeKind::set) {
            for (const sema::ConstValue& element : value->elements) {
                llvm::Value* key = spill(materialize(&element, type->inner), "key");
                builder_.CreateCall(runtimeFunction("bobTableAtPut", builder_.getVoidTy(), {ptr, ptr, ptr, ptr, ptr}),
                                    {slot, key, none, elementDescriptor(type->inner), none});
            }
        }
        else {
            for (const auto& [k, v] : value->entries) {
                llvm::Value* key = spill(materialize(&k, type->key), "key");
                llvm::Value* item = spill(materialize(&v, type->inner), "value");
                builder_.CreateCall(runtimeFunction("bobTableAtPut", builder_.getVoidTy(), {ptr, ptr, ptr, ptr, ptr}),
                                    {slot, key, item, elementDescriptor(type->key), elementDescriptor(type->inner)});
            }
        }
        return builder_.CreateLoad(ptr, slot);
    }
    default:
        return defaultValue(type);
    }
}

llvm::Value* Generator::emitCompileTimeSend(const Node* node, const sema::SendInfo& send)
{
    switch (send.kind) {
    case SendKind::constantValue:
        return materialize(send.value, send.resultType);
    case SendKind::metaInfo:
        return nullptr;
    case SendKind::compilerDiagnostic: {
        auto keyword = static_cast<const KeywordSend*>(node);
        int32_t kind = send.op == "error:" ? 0 : send.op == "warning:" ? 1 : 2;
        llvm::Value* text = emitAs(keyword->arguments[0], model_.types.string());
        llvm::Function* report = runtimeFunction("bobCompilerDiagnostic", builder_.getVoidTy(), {builder_.getInt32Ty(), builder_.getPtrTy(), builder_.getInt64Ty()});
        builder_.CreateCall(report, {builder_.getInt32(static_cast<uint32_t>(kind)), text,
                                     builder_.getInt64(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(node)))});
        return nullptr;
    }
    case SendKind::metaFields: {
        // An array of FieldMeta { name, typeName, offset, size }, built where it's used.
        const sema::ClassInfo* fieldMeta = send.classInfo;
        const Type* subject = sema::canonical(send.operandType);
        llvm::Type* ptr = builder_.getPtrTy();
        llvm::AllocaInst* slot = entryAlloca(ptr, "fields");
        builder_.CreateStore(llvm::ConstantPointerNull::get(builder_.getPtrTy()), slot);
        for (const sema::FieldInfo* field : send.fields) {
            uint64_t offset = subject->kind == TypeKind::record ? sema::fieldOffset(subject->record, field->index)
                                                                : sema::objectFieldOffset(subject->classInfo, field->index);
            llvm::Value* object = builder_.CreateCall(allocFunction_, {classGlobals_.at(fieldMeta)}, "fieldMeta");
            llvm::Value* values[] = {stringLiteral(field->name), stringLiteral(sema::typeName(field->type)), builder_.getInt64(offset),
                                     builder_.getInt64(sema::sizeOf(field->type))};
            for (size_t i = 0; i < 4; ++i) {
                builder_.CreateStore(values[i], fieldAddress(object, &fieldMeta->fields[i]));
            }
            builder_.CreateCall(initFunctions_.at(fieldMeta), {object, builder_.getInt64(0xF)});
            builder_.CreateCall(initsFunctions_.at(fieldMeta), {object});
            builder_.CreateCall(runtimeFunction("bobArrayPush", builder_.getVoidTy(), {ptr, ptr, ptr}),
                                {slot, spill(object, "field"), elementDescriptor(fieldMeta->type)});
        }
        return builder_.CreateLoad(ptr, slot);
    }
    default:
        return nullptr;
    }
}

}

namespace bob::codegen {

std::unique_ptr<llvm::Module> generateEvaluationModule(const ast::Unit* unit, const sema::SemanticModel& model, llvm::LLVMContext& context,
                                                       const std::string& dataLayout, const std::string& triple,
                                                       const sema::CompileTimeItem* item, const std::string& thunk)
{
    detail::Generator generator(model, context, dataLayout);
    generator.evaluate(item, thunk, triple);
    return generator.run(unit);
}

uint64_t llvmSizeOf(const sema::Type* type, const sema::SemanticModel& model, const std::string& dataLayout)
{
    llvm::LLVMContext context;
    detail::Generator generator(model, context, dataLayout);
    return generator.measure(type);
}

}
