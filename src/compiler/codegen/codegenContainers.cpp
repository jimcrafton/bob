// Phase 5: strings, arrays, fixed arrays, dictionaries, hashtables and sets.
// Containers are pointers to runtime buffers (null = empty) with value
// semantics: mutating sends take the variable's address so the runtime can
// copy a shared buffer first. Every send whose result needs cleanup returns
// an owned (+1) value, like message sends.

#include "codegenInternal.h"

namespace bob::codegen::detail {

namespace {

llvm::Type* i8(llvm::IRBuilder<>& b)
{
    return b.getInt8Ty();
}

}

// An immortal constant { class, refCount, flags = immortal, byteCount, bytes }.
llvm::Constant* Generator::stringLiteral(const std::string& text)
{
    if (text.empty()) {
        return llvm::ConstantPointerNull::get(builder_.getPtrTy());
    }
    auto it = stringLiterals_.find(text);
    if (it != stringLiterals_.end()) {
        return it->second;
    }
    llvm::Constant* cls = module_->getOrInsertGlobal("bobStringClass", builder_.getInt8Ty());
    llvm::Constant* bytes = llvm::ConstantDataArray::getString(context_, text, false);
    llvm::StructType* layout = llvm::StructType::get(context_, {builder_.getPtrTy(), builder_.getInt32Ty(), builder_.getInt32Ty(),
                                                                builder_.getInt64Ty(), bytes->getType()});
    llvm::Constant* value = llvm::ConstantStruct::get(
        layout, {cls, builder_.getInt32(1), builder_.getInt32(bobObjectImmortal), builder_.getInt64(text.size()), bytes});
    auto global = new llvm::GlobalVariable(*module_, layout, true, llvm::GlobalValue::PrivateLinkage, value,
                                           "bob.string." + std::to_string(stringLiterals_.size()));
    global->setAlignment(llvm::Align(8));
    stringLiterals_[text] = global;
    return global;
}

llvm::Value* Generator::printString(llvm::Value* value, const Type* type)
{
    type = sema::decay(sema::canonical(type));
    llvm::Type* ptr = builder_.getPtrTy();
    if (sema::isString(type)) {
        retainValue(value, type);
        return value;
    }
    if (sema::isInteger(type)) {
        llvm::Value* wide = type->isSigned ? builder_.CreateSExt(value, builder_.getInt64Ty()) : builder_.CreateZExtOrTrunc(value, builder_.getInt64Ty());
        return builder_.CreateCall(runtimeFunction("bobStringFromInteger", ptr, {builder_.getInt64Ty(), builder_.getInt32Ty()}),
                                   {wide, builder_.getInt32(type->isSigned ? 0 : 1)});
    }
    if (sema::isReal(type)) {
        llvm::Value* wide = type->bits == 32 ? builder_.CreateFPExt(value, builder_.getDoubleTy()) : value;
        return builder_.CreateCall(runtimeFunction("bobStringFromReal", ptr, {builder_.getDoubleTy()}), {wide});
    }
    if (type->kind == TypeKind::boolType) {
        return builder_.CreateCall(runtimeFunction("bobStringFromBool", ptr, {i8(builder_)}), {builder_.CreateZExt(value, i8(builder_))});
    }
    if (type->kind == TypeKind::character) {
        return builder_.CreateCall(runtimeFunction("bobStringFromChar", ptr, {builder_.getInt32Ty()}), {value});
    }
    if (sema::isEnum(type)) {
        llvm::GlobalVariable*& table = enumNameTables_[type->enumInfo];
        if (table == nullptr) {
            std::vector<llvm::Constant*> names;
            for (const sema::EnumMemberInfo& member : type->enumInfo->members) {
                names.push_back(stringLiteral(member.name));
            }
            llvm::ArrayType* arrayType = llvm::ArrayType::get(ptr, names.size());
            table = new llvm::GlobalVariable(*module_, arrayType, true, llvm::GlobalValue::PrivateLinkage, llvm::ConstantArray::get(arrayType, names),
                                             "bob." + type->enumInfo->name + ".names");
        }
        llvm::Value* ordinal = enumOrdinal(value, type->enumInfo);
        return builder_.CreateLoad(ptr, builder_.CreateGEP(table->getValueType(), table, {builder_.getInt32(0), ordinal}));
    }
    return llvm::ConstantPointerNull::get(builder_.getPtrTy());
}

llvm::Value* Generator::emitStringSend(const Node* node, const sema::SendInfo& send)
{
    llvm::Type* ptr = builder_.getPtrTy();
    llvm::Type* i64 = builder_.getInt64Ty();
    llvm::Type* i32 = builder_.getInt32Ty();
    const Type* string = model_.types.string();
    auto unary = nodeCast<UnarySend>(node);
    auto binary = nodeCast<BinarySend>(node);
    auto keyword = nodeCast<KeywordSend>(node);
    const Node* receiver = unary != nullptr ? unary->receiver : binary != nullptr ? binary->receiver : keyword != nullptr ? keyword->receiver : nullptr;
    switch (send.kind) {
    case SendKind::stringConcat:
        return builder_.CreateCall(runtimeFunction("bobStringConcat", ptr, {ptr, ptr}), {emitAs(receiver, string), emitAs(binary->argument, string)});
    case SendKind::stringCompare: {
        llvm::Value* order = builder_.CreateCall(runtimeFunction("bobStringCompare", i32, {ptr, ptr}),
                                                 {emitAs(receiver, string), emitAs(binary->argument, string)});
        return compare(send.op, order, builder_.getInt32(0), model_.types.int32());
    }
    case SendKind::stringView: {
        llvm::Value* value = emitAs(receiver, string);
        retainValue(value, string);
        return value;
    }
    case SendKind::stringViewCount: {
        llvm::Value* value = emitAs(receiver, send.operandType);
        const char* name = send.op == "bytes" ? "bobStringByteCount" : send.op == "codepoints" ? "bobStringCodepointCount" : "bobStringGraphemeCount";
        return builder_.CreateCall(runtimeFunction(name, i64, {ptr}), {value});
    }
    case SendKind::stringByteAt: {
        llvm::Value* value = emitAs(receiver, model_.typeOf(receiver));
        llvm::Value* index = emitAs(keyword->arguments[0], send.operandType);
        index = send.operandType->isSigned ? builder_.CreateSExtOrTrunc(index, i64) : builder_.CreateZExtOrTrunc(index, i64);
        return builder_.CreateCall(runtimeFunction("bobStringByteAt", i8(builder_), {ptr, i64}), {value, index});
    }
    case SendKind::stringIsEmpty:
        return builder_.CreateICmpEQ(emitAs(receiver, string), llvm::ConstantPointerNull::get(builder_.getPtrTy()));
    case SendKind::stringPredicate: {
        int kind = send.op == "startsWith:" ? 0 : send.op == "endsWith:" ? 1 : 2;
        llvm::Value* result = builder_.CreateCall(runtimeFunction("bobStringPredicate", i8(builder_), {ptr, ptr, i32}),
                                                  {emitAs(receiver, string), emitAs(keyword->arguments[0], string), builder_.getInt32(kind)});
        return builder_.CreateICmpNE(result, builder_.getInt8(0));
    }
    case SendKind::stringSplit:
        return builder_.CreateCall(runtimeFunction("bobStringSplit", ptr, {ptr, ptr}), {emitAs(receiver, string), emitAs(keyword->arguments[0], string)});
    case SendKind::stringJoin:
        return builder_.CreateCall(runtimeFunction("bobStringJoin", ptr, {ptr, ptr}),
                                   {emitAs(receiver, string), emitAs(keyword->arguments[0], model_.typeOf(keyword->arguments[0]))});
    case SendKind::stringFromUtf8:
    case SendKind::stringFromUtf8Lossy: {
        bool lossy = send.kind == SendKind::stringFromUtf8Lossy;
        llvm::AllocaInst* ok = entryAlloca(i32, "utf8.ok");
        llvm::Value* bytes = emitAs(keyword->arguments[0], model_.typeOf(keyword->arguments[0]));
        llvm::Value* result = builder_.CreateCall(runtimeFunction("bobStringFromUtf8", ptr, {ptr, i32, ptr}), {bytes, builder_.getInt32(lossy ? 1 : 0), ok});
        if (lossy) {
            return result;
        }
        llvm::Value* valid = builder_.CreateICmpNE(builder_.CreateLoad(i32, ok), builder_.getInt32(0));
        return makeOptional(valid, result, send.resultType);
    }
    case SendKind::stringToUtf8:
        return builder_.CreateCall(runtimeFunction("bobStringToUtf8", ptr, {ptr}), {emitAs(receiver, string)});
    case SendKind::printString:
        return printString(emitAs(receiver, send.operandType), send.operandType);
    case SendKind::charValue:
        return emitAs(receiver, send.operandType);
    case SendKind::charFromValue: {
        llvm::Value* value = emitAs(keyword->arguments[0], model_.types.integer(32, false));
        llvm::Value* surrogate = builder_.CreateAnd(builder_.CreateICmpUGE(value, builder_.getInt32(0xD800)), builder_.CreateICmpULE(value, builder_.getInt32(0xDFFF)));
        llvm::Value* ok = builder_.CreateAnd(builder_.CreateICmpULE(value, builder_.getInt32(0x10FFFF)), builder_.CreateNot(surrogate));
        return makeOptional(ok, value, send.resultType);
    }
    case SendKind::stringFromChar:
        return builder_.CreateCall(runtimeFunction("bobStringFromChar", ptr, {i32}), {emitAs(receiver, send.operandType)});
    default:
        return nullptr;
    }
}

// ---------------------------------------------------------------------------
// container helpers

llvm::Value* Generator::mutableSlot(const Node* receiver)
{
    return emitAddress(receiver);
}

llvm::Value* Generator::arrayCount(llvm::Value* array)
{
    llvm::BasicBlock* before = builder_.GetInsertBlock();
    llvm::BasicBlock* load = llvm::BasicBlock::Create(context_, "count.load", function_);
    llvm::BasicBlock* done = llvm::BasicBlock::Create(context_, "count.done", function_);
    builder_.CreateCondBr(builder_.CreateICmpEQ(array, llvm::ConstantPointerNull::get(builder_.getPtrTy())), done, load);
    builder_.SetInsertPoint(load);
    llvm::Value* address = builder_.CreateConstInBoundsGEP1_64(builder_.getInt8Ty(), array, abi::arrayCountOffset);
    llvm::Value* count = builder_.CreateLoad(builder_.getInt64Ty(), address, "count");
    builder_.CreateBr(done);
    builder_.SetInsertPoint(done);
    llvm::PHINode* phi = builder_.CreatePHI(builder_.getInt64Ty(), 2);
    phi->addIncoming(builder_.getInt64(0), before);
    phi->addIncoming(count, load);
    return phi;
}

llvm::Value* Generator::arrayElementAddress(llvm::Value* array, llvm::Value* index, const Type* element)
{
    llvm::Value* data = builder_.CreateConstInBoundsGEP1_64(builder_.getInt8Ty(), array, abi::arrayDataOffset);
    return builder_.CreateInBoundsGEP(llvmType(element), data, {index});
}

llvm::Value* Generator::checkedIndex(llvm::Value* index, const Type* indexType, llvm::Value* count)
{
    indexType = sema::decay(indexType);
    llvm::Value* wide = indexType->isSigned ? builder_.CreateSExtOrTrunc(index, builder_.getInt64Ty()) : builder_.CreateZExtOrTrunc(index, builder_.getInt64Ty());
    // Unsigned compare: a negative index is huge, so one test covers both ends.
    trapIf(builder_.CreateICmpUGE(wide, count), abi::TrapReason::indexOutOfRange);
    return wide;
}

llvm::Value* Generator::fixedIndex(llvm::Value* index, const Type* fixedType)
{
    const Type* key = sema::canonical(fixedType->key);
    llvm::Type* i64 = builder_.getInt64Ty();
    if (sema::isEnum(key)) {
        return builder_.CreateZExt(enumOrdinal(index, key->enumInfo), i64);
    }
    if (sema::isEnum(key->inner)) {
        return builder_.CreateZExt(builder_.CreateSub(enumOrdinal(index, key->enumInfo), builder_.getInt32(static_cast<uint32_t>(key->low))), i64);
    }
    llvm::Value* wide = key->isSigned ? builder_.CreateSExt(index, i64) : builder_.CreateZExt(index, i64);
    return builder_.CreateSub(wide, builder_.getInt64(static_cast<uint64_t>(key->low)));
}

llvm::Value* Generator::emitArrayLiteral(const ArrayLiteral* literal, const sema::SendInfo& send)
{
    const Type* type = sema::canonical(send.resultType);
    llvm::Type* ptr = builder_.getPtrTy();
    if (send.kind == SendKind::fixedArrayLiteral) {
        llvm::Value* value = llvm::UndefValue::get(llvmType(type));
        for (size_t i = 0; i < literal->elements.size() && i < type->count; ++i) {
            value = builder_.CreateInsertValue(value, emitOwnedAs(literal->elements[i], type->inner), {static_cast<unsigned>(i)});
        }
        return value;
    }
    if (literal->elements.empty() || type->pending) {
        return llvm::ConstantPointerNull::get(builder_.getPtrTy());
    }
    llvm::AllocaInst* slot = entryAlloca(ptr, "literal");
    builder_.CreateStore(llvm::ConstantPointerNull::get(builder_.getPtrTy()), slot);
    llvm::Constant* descriptor = elementDescriptor(type->inner);
    for (const Node* element : literal->elements) {
        llvm::Value* owned = spill(emitOwnedAs(element, type->inner), "element");
        if (send.kind == SendKind::setLiteral) {
            builder_.CreateCall(runtimeFunction("bobTableAtPut", builder_.getVoidTy(), {ptr, ptr, ptr, ptr, ptr}),
                                {slot, owned, llvm::ConstantPointerNull::get(builder_.getPtrTy()), descriptor, llvm::ConstantPointerNull::get(builder_.getPtrTy())});
        }
        else {
            builder_.CreateCall(runtimeFunction("bobArrayPush", builder_.getVoidTy(), {ptr, ptr, ptr}), {slot, owned, descriptor});
        }
    }
    return builder_.CreateLoad(ptr, slot);
}

// ---------------------------------------------------------------------------
// container sends

llvm::Value* Generator::emitContainerSend(const Node* node, const sema::SendInfo& send)
{
    llvm::Type* ptr = builder_.getPtrTy();
    llvm::Type* i64 = builder_.getInt64Ty();
    llvm::Type* voidType = builder_.getVoidTy();
    llvm::Constant* none = llvm::ConstantPointerNull::get(builder_.getPtrTy());
    auto unary = nodeCast<UnarySend>(node);
    auto keyword = nodeCast<KeywordSend>(node);
    const Node* receiver = unary != nullptr ? unary->receiver : keyword != nullptr ? keyword->receiver : nullptr;
    const Type* type = sema::canonical(send.operandType);
    auto isTable = [](const Type* t) {
        return t->kind == TypeKind::dictionary || t->kind == TypeKind::hashtable || (t->kind == TypeKind::set && !sema::isBitSet(t));
    };
    auto valueDescriptor = [&](const Type* t) -> llvm::Constant* {
        return t->kind == TypeKind::set ? static_cast<llvm::Constant*>(none) : elementDescriptor(t->inner);
    };
    auto keyType = [&](const Type* t) { return t->kind == TypeKind::set ? t->inner : t->key; };
    // Loads a copy of the element at `address` (owned).
    auto loadOwned = [&](llvm::Value* address, const Type* element) {
        llvm::Value* value = builder_.CreateLoad(llvmType(element), address);
        retainValue(value, element);
        return value;
    };

    switch (send.kind) {
    case SendKind::arrayLiteral:
    case SendKind::setLiteral:
    case SendKind::fixedArrayLiteral:
        return emitArrayLiteral(static_cast<const ArrayLiteral*>(node), send);
    case SendKind::containerCount:
    case SendKind::containerIsEmpty: {
        llvm::Value* count = nullptr;
        if (type->kind == TypeKind::fixedArray) {
            count = builder_.getInt64(type->count);
        }
        else if (isTable(type)) {
            count = builder_.CreateCall(runtimeFunction("bobTableCount", i64, {ptr}), {emitAs(receiver, type)});
        }
        else {
            count = arrayCount(emitAs(receiver, type));
        }
        return send.kind == SendKind::containerCount ? count : builder_.CreateICmpEQ(count, builder_.getInt64(0));
    }
    case SendKind::containerAt:
    case SendKind::containerAtIfAbsent: {
        if (type->kind == TypeKind::array) {
            llvm::Value* array = emitAs(receiver, type);
            llvm::Value* index = checkedIndex(emitAs(keyword->arguments[0], model_.typeOf(keyword->arguments[0])), model_.typeOf(keyword->arguments[0]),
                                              arrayCount(array));
            return loadOwned(arrayElementAddress(array, index, type->inner), type->inner);
        }
        if (type->kind == TypeKind::fixedArray) {
            llvm::Value* index = fixedIndex(emitAs(keyword->arguments[0], type->key), type);
            llvm::Value* base = emitAddress(receiver);
            if (base == nullptr) {
                base = spill(emitAs(receiver, type), "fixed");
            }
            return loadOwned(builder_.CreateInBoundsGEP(llvmType(type), base, {builder_.getInt64(0), index}), type->inner);
        }
        const Type* value = type->inner;
        llvm::Value* table = emitAs(receiver, type);
        llvm::Value* key = spill(emitAs(keyword->arguments[0], keyType(type)), "key");
        llvm::Value* found = builder_.CreateCall(runtimeFunction("bobTableAt", ptr, {ptr, ptr}), {table, key});
        llvm::BasicBlock* hit = llvm::BasicBlock::Create(context_, "at.found", function_);
        llvm::BasicBlock* miss = llvm::BasicBlock::Create(context_, "at.missing", function_);
        llvm::BasicBlock* done = llvm::BasicBlock::Create(context_, "at.done", function_);
        builder_.CreateCondBr(builder_.CreateICmpNE(found, none), hit, miss);
        const Type* result = sema::canonical(send.resultType);
        builder_.SetInsertPoint(hit);
        llvm::Value* loaded = loadOwned(found, value);
        if (send.kind == SendKind::containerAt && !sema::isOptional(value)) {
            loaded = makeOptional(builder_.getTrue(), loaded, result);
        }
        hit = builder_.GetInsertBlock();
        builder_.CreateBr(done);
        builder_.SetInsertPoint(miss);
        llvm::Value* fallback = nullptr;
        if (send.kind == SendKind::containerAtIfAbsent) {
            size_t mark = temporaries_.size();
            fallback = emitInlinedBody(static_cast<const BlockLiteral*>(keyword->arguments[1]), value);
            flushTemporaries(mark);
        }
        else {
            fallback = defaultValue(result);
        }
        miss = builder_.GetInsertBlock();
        bool missReturns = blockTerminated();
        if (!missReturns) {
            builder_.CreateBr(done);
        }
        builder_.SetInsertPoint(done);
        llvm::PHINode* phi = builder_.CreatePHI(llvmType(result), 2);
        phi->addIncoming(loaded, hit);
        if (!missReturns) {
            phi->addIncoming(fallback, miss);
        }
        return phi;
    }
    case SendKind::containerAtPut: {
        if (type->kind == TypeKind::fixedArray) {
            llvm::Value* address = emitAddress(receiver);
            llvm::Value* index = fixedIndex(emitAs(keyword->arguments[0], type->key), type);
            llvm::Value* value = emitOwnedAs(keyword->arguments[1], type->inner);
            storeReference(builder_.CreateInBoundsGEP(llvmType(type), address, {builder_.getInt64(0), index}), value, type->inner);
            return nullptr;
        }
        llvm::Value* slot = mutableSlot(receiver);
        if (type->kind == TypeKind::array) {
            const Type* indexType = model_.typeOf(keyword->arguments[0]);
            llvm::Value* index = checkedIndex(emitAs(keyword->arguments[0], indexType), indexType, arrayCount(builder_.CreateLoad(ptr, slot)));
            llvm::Value* value = emitOwnedAs(keyword->arguments[1], type->inner);
            llvm::Value* data = builder_.CreateCall(runtimeFunction("bobArrayMakeUnique", ptr, {ptr, ptr}), {slot, elementDescriptor(type->inner)});
            storeReference(builder_.CreateInBoundsGEP(llvmType(type->inner), data, {index}), value, type->inner);
            return nullptr;
        }
        llvm::Value* key = spill(emitOwnedAs(keyword->arguments[0], keyType(type)), "key");
        llvm::Value* value = spill(emitOwnedAs(keyword->arguments[1], type->inner), "value");
        builder_.CreateCall(runtimeFunction("bobTableAtPut", voidType, {ptr, ptr, ptr, ptr, ptr}),
                            {slot, key, value, elementDescriptor(keyType(type)), valueDescriptor(type)});
        return nullptr;
    }
    case SendKind::containerIncludes: {
        llvm::Value* container = emitAs(receiver, type);
        if (type->kind == TypeKind::array) {
            llvm::Value* element = spill(emitAs(keyword->arguments[0], type->inner), "element");
            llvm::Value* found = builder_.CreateCall(runtimeFunction("bobArrayIncludes", builder_.getInt8Ty(), {ptr, ptr}), {container, element});
            return builder_.CreateICmpNE(found, builder_.getInt8(0));
        }
        llvm::Value* key = spill(emitAs(keyword->arguments[0], keyType(type)), "key");
        llvm::Value* found = builder_.CreateCall(runtimeFunction("bobTableAt", ptr, {ptr, ptr}), {container, key});
        return builder_.CreateICmpNE(found, none);
    }
    case SendKind::containerAdd: {
        llvm::Value* slot = mutableSlot(receiver);
        llvm::Value* element = spill(emitOwnedAs(keyword->arguments[0], type->inner), "element");
        if (type->kind == TypeKind::array) {
            builder_.CreateCall(runtimeFunction("bobArrayPush", voidType, {ptr, ptr, ptr}), {slot, element, elementDescriptor(type->inner)});
        }
        else {
            builder_.CreateCall(runtimeFunction("bobTableAtPut", voidType, {ptr, ptr, ptr, ptr, ptr}),
                                {slot, element, none, elementDescriptor(type->inner), none});
        }
        return nullptr;
    }
    case SendKind::containerRemove: {
        llvm::Value* slot = mutableSlot(receiver);
        llvm::Value* key = spill(emitAs(keyword->arguments[0], keyType(type)), "key");
        builder_.CreateCall(runtimeFunction("bobTableRemove", builder_.getInt8Ty(), {ptr, ptr, ptr, ptr}),
                            {slot, key, elementDescriptor(keyType(type)), valueDescriptor(type)});
        return nullptr;
    }
    case SendKind::arrayFirst:
    case SendKind::arrayLast: {
        llvm::Value* array = emitAs(receiver, type);
        llvm::Value* count = arrayCount(array);
        trapIf(builder_.CreateICmpEQ(count, builder_.getInt64(0)), abi::TrapReason::indexOutOfRange);
        llvm::Value* index = send.kind == SendKind::arrayFirst ? builder_.getInt64(0) : builder_.CreateSub(count, builder_.getInt64(1));
        return loadOwned(arrayElementAddress(array, index, type->inner), type->inner);
    }
    case SendKind::arrayRemoveLast: {
        llvm::Value* slot = mutableSlot(receiver);
        llvm::AllocaInst* out = entryAlloca(llvmType(type->inner), "removed");
        builder_.CreateCall(runtimeFunction("bobArrayRemoveLast", voidType, {ptr, ptr, ptr}), {slot, out, elementDescriptor(type->inner)});
        return builder_.CreateLoad(llvmType(type->inner), out);
    }
    case SendKind::dictionaryKeys:
        return builder_.CreateCall(runtimeFunction("bobTableKeys", ptr, {ptr}), {emitAs(receiver, type)});
    case SendKind::dictionaryValues:
        return builder_.CreateCall(runtimeFunction("bobTableValues", ptr, {ptr}), {emitAs(receiver, type)});
    case SendKind::inlineDo: {
        auto literal = static_cast<const BlockLiteral*>(keyword->arguments[0]);
        const sema::BlockInfo* info = model_.blockOf(literal);
        std::vector<const sema::LocalInfo*> locals(info->params.begin(), info->params.end());
        emitCollectionLoop(receiver, model_.typeOf(receiver), locals, literal->statements, send.op);
        return nullptr;
    }
    default:
        return nullptr;
    }
}

// ---------------------------------------------------------------------------
// iteration

void Generator::emitCollectionLoop(const Node* collection, const Type* type, const std::vector<const sema::LocalInfo*>& locals,
                                   const std::vector<Node*>& body, const std::string& selector)
{
    type = sema::canonical(type);
    llvm::Type* ptr = builder_.getPtrTy();
    llvm::Type* i64 = builder_.getInt64Ty();
    if (sema::isBitSet(type)) {
        llvm::Value* mask = emitAs(collection, type);
        const Type* element = type->inner;
        emitOrdinalLoop(locals[0], element, builder_.getInt64(0), builder_.getInt64(element->enumInfo->members.size() - 1), body, mask);
        return;
    }
    // The loop holds its own reference, so the body may reassign the variable.
    llvm::Value* source = emitOwnedAs(collection, type);
    addTemporary(source, type);

    auto bind = [&](const sema::LocalInfo* local, llvm::Value* owned, const Type* valueType) {
        storeReference(addressOf(local), widen(owned, valueType, local->type), local->type);
    };
    auto loadOwned = [&](llvm::Value* address, const Type* element) {
        llvm::Value* value = builder_.CreateLoad(llvmType(element), address);
        retainValue(value, element);
        return value;
    };

    llvm::BasicBlock* condition = llvm::BasicBlock::Create(context_, "loop.cond", function_);
    llvm::BasicBlock* bodyBlock = llvm::BasicBlock::Create(context_, "loop.body", function_);
    llvm::BasicBlock* step = llvm::BasicBlock::Create(context_, "loop.step", function_);
    llvm::BasicBlock* exit = llvm::BasicBlock::Create(context_, "loop.end", function_);
    llvm::AllocaInst* position = entryAlloca(i64, "position");
    llvm::Value* limit = nullptr;
    std::function<void()> advance;

    if (sema::isStringView(type)) {
        limit = builder_.CreateCall(runtimeFunction("bobStringByteCount", i64, {ptr}), {source});
        builder_.CreateStore(builder_.getInt64(0), position);
        builder_.CreateBr(condition);
        builder_.SetInsertPoint(condition);
        builder_.CreateCondBr(builder_.CreateICmpSLT(builder_.CreateLoad(i64, position), limit), bodyBlock, exit);
        builder_.SetInsertPoint(bodyBlock);
        llvm::Value* at = builder_.CreateLoad(i64, position);
        if (type->count == 0) {
            bind(locals[0], builder_.CreateCall(runtimeFunction("bobStringByteAt", builder_.getInt8Ty(), {ptr, i64}), {source, at}), type->inner);
            builder_.CreateStore(builder_.CreateAdd(at, builder_.getInt64(1)), position);
        }
        else if (type->count == 1) {
            bind(locals[0], builder_.CreateCall(runtimeFunction("bobStringDecodeAt", builder_.getInt32Ty(), {ptr, ptr}), {source, position}), type->inner);
        }
        else {
            llvm::Value* end = builder_.CreateCall(runtimeFunction("bobStringNextGrapheme", i64, {ptr, i64}), {source, at});
            bind(locals[0], builder_.CreateCall(runtimeFunction("bobStringSlice", ptr, {ptr, i64, i64}), {source, at, end}), type->inner);
            builder_.CreateStore(end, position);
        }
        // The position already moved past the element, so `continue` re-tests.
        advance = [] {};
        step->eraseFromParent();
        step = condition;
    }
    else if (type->kind == TypeKind::array || type->kind == TypeKind::fixedArray) {
        llvm::Value* base = source;
        if (type->kind == TypeKind::fixedArray) {
            base = spill(source, "fixed");
            limit = builder_.getInt64(type->count);
        }
        else {
            limit = arrayCount(source);
        }
        builder_.CreateStore(builder_.getInt64(0), position);
        builder_.CreateBr(condition);
        builder_.SetInsertPoint(condition);
        builder_.CreateCondBr(builder_.CreateICmpULT(builder_.CreateLoad(i64, position), limit), bodyBlock, exit);
        builder_.SetInsertPoint(bodyBlock);
        llvm::Value* at = builder_.CreateLoad(i64, position);
        llvm::Value* address = type->kind == TypeKind::fixedArray ? builder_.CreateInBoundsGEP(llvmType(type), base, {builder_.getInt64(0), at})
                                                                   : arrayElementAddress(base, at, type->inner);
        bind(locals[0], loadOwned(address, type->inner), type->inner);
        advance = [&] { builder_.CreateStore(builder_.CreateAdd(builder_.CreateLoad(i64, position), builder_.getInt64(1)), position); };
    }
    else if (type->kind == TypeKind::slice) {
        llvm::Value* base = builder_.CreateExtractValue(source, {0}, "slice.base");
        limit = builder_.CreateExtractValue(source, {1}, "slice.count");
        builder_.CreateStore(builder_.getInt64(0), position);
        builder_.CreateBr(condition);
        builder_.SetInsertPoint(condition);
        builder_.CreateCondBr(builder_.CreateICmpULT(builder_.CreateLoad(i64, position), limit), bodyBlock, exit);
        builder_.SetInsertPoint(bodyBlock);
        llvm::Value* address = builder_.CreateInBoundsGEP(llvmType(type->inner), base, {builder_.CreateLoad(i64, position)});
        bind(locals[0], builder_.CreateLoad(llvmType(type->inner), address), type->inner);
        advance = [&] { builder_.CreateStore(builder_.CreateAdd(builder_.CreateLoad(i64, position), builder_.getInt64(1)), position); };
    }
    else {
        // Tables: visit occupied entries in storage order.
        llvm::Function* next = runtimeFunction("bobTableNext", i64, {ptr, i64});
        builder_.CreateStore(builder_.CreateCall(next, {source, builder_.getInt64(0)}), position);
        builder_.CreateBr(condition);
        builder_.SetInsertPoint(condition);
        builder_.CreateCondBr(builder_.CreateICmpSGE(builder_.CreateLoad(i64, position), builder_.getInt64(0)), bodyBlock, exit);
        builder_.SetInsertPoint(bodyBlock);
        llvm::Value* at = builder_.CreateLoad(i64, position);
        bool isSet = type->kind == TypeKind::set;
        const Type* key = isSet ? type->inner : type->key;
        llvm::Value* keyAddress = builder_.CreateCall(runtimeFunction("bobTableKeyAt", ptr, {ptr, i64}), {source, at});
        if (isSet || selector == "keysDo:") {
            bind(locals[0], loadOwned(keyAddress, key), key);
        }
        else {
            llvm::Value* valueAddress = builder_.CreateCall(runtimeFunction("bobTableValueAt", ptr, {ptr, i64}), {source, at});
            if (selector == "keysAndValuesDo:") {
                bind(locals[0], loadOwned(keyAddress, key), key);
                bind(locals[1], loadOwned(valueAddress, type->inner), type->inner);
            }
            else {
                bind(locals[0], loadOwned(valueAddress, type->inner), type->inner);
            }
        }
        advance = [&, next] {
            llvm::Value* current = builder_.CreateLoad(i64, position);
            builder_.CreateStore(builder_.CreateCall(next, {source, builder_.CreateAdd(current, builder_.getInt64(1))}), position);
        };
    }

    loops_.push_back({exit, step});
    size_t mark = temporaries_.size();
    emitStatements(body);
    flushTemporaries(mark);
    loops_.pop_back();
    if (!blockTerminated()) {
        builder_.CreateBr(step);
    }
    if (step != condition) {
        builder_.SetInsertPoint(step);
        advance();
        builder_.CreateBr(condition);
    }
    builder_.SetInsertPoint(exit);
}

}
