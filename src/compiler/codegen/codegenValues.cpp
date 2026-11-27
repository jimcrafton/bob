// Phase 5 values: optionals, unions, enums, subranges, enum sets, and reference
// counting of values that hold managed pointers.

#include "codegenInternal.h"

namespace bob::codegen::detail {

llvm::Function* Generator::runtimeFunction(const char* name, llvm::Type* result, std::vector<llvm::Type*> params)
{
    if (llvm::Function* existing = module_->getFunction(name)) {
        return existing;
    }
    return llvm::Function::Create(llvm::FunctionType::get(result, params, false), llvm::GlobalValue::ExternalLinkage, name, module_.get());
}

uint64_t Generator::sizeOf(const Type* type)
{
    return module_->getDataLayout().getTypeAllocSize(llvmType(type));
}

llvm::Constant* Generator::defaultValue(const Type* type)
{
    type = sema::canonical(type);
    llvm::Type* llvm = llvmType(type);
    switch (type->kind) {
    case TypeKind::enumType:
        return llvm::ConstantInt::get(llvm, type->enumInfo->members.front().value);
    case TypeKind::subrange:
        if (sema::isEnum(type->inner)) {
            return llvm::ConstantInt::get(llvm, type->enumInfo->members[static_cast<size_t>(type->low)].value);
        }
        return llvm::ConstantInt::get(llvm, static_cast<uint64_t>(type->low), type->isSigned);
    case TypeKind::record: {
        std::vector<llvm::Constant*> fields;
        for (const sema::FieldInfo& field : type->record->fields) {
            fields.push_back(defaultValue(field.type));
        }
        return llvm::ConstantStruct::get(llvm::cast<llvm::StructType>(llvm), fields);
    }
    case TypeKind::fixedArray:
        return llvm::ConstantArray::get(llvm::cast<llvm::ArrayType>(llvm),
                                        std::vector<llvm::Constant*>(type->count, defaultValue(type->inner)));
    default:
        return llvm::Constant::getNullValue(llvm);
    }
}

// ---------------------------------------------------------------------------
// reference counting of values

void Generator::addTemporary(llvm::Value* value, const Type* type)
{
    temporaries_.push_back({value, type});
}

void Generator::retainValue(llvm::Value* value, const Type* type)
{
    type = sema::canonical(type);
    if (value == nullptr || !sema::needsCleanup(type) || llvm::isa<llvm::ConstantPointerNull>(value) ||
        llvm::isa<llvm::ConstantAggregateZero>(value)) {
        return;
    }
    if (sema::isManaged(type)) {
        builder_.CreateCall(retainFunction_, {value});
        return;
    }
    if (sema::isOptional(type)) {
        // An absent optional holds a zero value, which is safe to retain.
        retainValue(builder_.CreateExtractValue(value, {1}), type->inner);
        return;
    }
    builder_.CreateCall(elementCallback(type, true), {spill(value, "retain.tmp")});
}

void Generator::releaseValue(llvm::Value* value, const Type* type)
{
    type = sema::canonical(type);
    if (value == nullptr || !sema::needsCleanup(type) || llvm::isa<llvm::ConstantPointerNull>(value) ||
        llvm::isa<llvm::ConstantAggregateZero>(value)) {
        return;
    }
    if (sema::isManaged(type)) {
        builder_.CreateCall(releaseFunction_, {value});
        return;
    }
    if (sema::isOptional(type)) {
        releaseValue(builder_.CreateExtractValue(value, {1}), type->inner);
        return;
    }
    builder_.CreateCall(elementCallback(type, false), {spill(value, "release.tmp")});
}

llvm::Value* Generator::spill(llvm::Value* value, const std::string& name)
{
    llvm::AllocaInst* slot = entryAlloca(value->getType(), name);
    builder_.CreateStore(value, slot);
    return slot;
}

// `void callback(ptr element)`: retains or releases the value of `type` stored there.
llvm::Function* Generator::elementCallback(const Type* type, bool retain)
{
    type = sema::canonical(type);
    auto& cache = retain ? retainCallbacks_ : releaseCallbacks_;
    auto it = cache.find(type);
    if (it != cache.end()) {
        return it->second;
    }
    auto* function = llvm::Function::Create(llvm::FunctionType::get(builder_.getVoidTy(), {builder_.getPtrTy()}, false),
                                            llvm::GlobalValue::InternalLinkage,
                                            std::string("bob.") + (retain ? "retain." : "release.") + std::to_string(cache.size()), module_.get());
    cache[type] = function;

    llvm::Function* savedFunction = function_;
    llvm::IRBuilderBase::InsertPoint savedPoint = builder_.saveIP();
    function_ = function;
    builder_.SetInsertPoint(llvm::BasicBlock::Create(context_, "entry", function));
    llvm::Value* address = function->getArg(0);
    auto visit = [&](llvm::Value* value, const Type* inner) {
        if (retain) {
            retainValue(value, inner);
        }
        else {
            releaseValue(value, inner);
        }
    };
    if (type->kind == TypeKind::fixedArray) {
        llvm::Type* element = llvmType(type->inner);
        llvm::BasicBlock* loop = llvm::BasicBlock::Create(context_, "loop", function);
        llvm::BasicBlock* done = llvm::BasicBlock::Create(context_, "done", function);
        llvm::BasicBlock* entry = builder_.GetInsertBlock();
        builder_.CreateBr(loop);
        builder_.SetInsertPoint(loop);
        llvm::PHINode* index = builder_.CreatePHI(builder_.getInt64Ty(), 2);
        index->addIncoming(builder_.getInt64(0), entry);
        llvm::Value* at = builder_.CreateGEP(element, address, {index});
        visit(builder_.CreateLoad(element, at), type->inner);
        llvm::Value* next = builder_.CreateAdd(index, builder_.getInt64(1));
        index->addIncoming(next, builder_.GetInsertBlock());
        builder_.CreateCondBr(builder_.CreateICmpEQ(next, builder_.getInt64(type->count)), done, loop);
        builder_.SetInsertPoint(done);
    }
    else if (sema::isUnion(type)) {
        llvm::Value* value = builder_.CreateLoad(llvmType(type), address);
        switchOnUnion(value, type, nullptr, [&](size_t member, llvm::Value* memberValue) -> llvm::Value* {
            visit(memberValue, type->members[member]);
            return nullptr;
        });
    }
    else {
        visit(builder_.CreateLoad(llvmType(type), address), type);
    }
    builder_.CreateRetVoid();
    function_ = savedFunction;
    builder_.restoreIP(savedPoint);
    return function;
}

llvm::Constant* Generator::elementDescriptor(const Type* type)
{
    type = sema::canonical(type);
    auto it = descriptors_.find(type);
    if (it != descriptors_.end()) {
        return it->second;
    }
    llvm::Type* ptr = builder_.getPtrTy();
    llvm::StructType* layout = llvm::StructType::get(context_, {builder_.getInt64Ty(), ptr, ptr, builder_.getInt32Ty(), builder_.getInt32Ty()});
    bool managed = sema::needsCleanup(type);
    llvm::Constant* retain = managed ? static_cast<llvm::Constant*>(elementCallback(type, true)) : llvm::ConstantPointerNull::get(builder_.getPtrTy());
    llvm::Constant* release = managed ? static_cast<llvm::Constant*>(elementCallback(type, false)) : llvm::ConstantPointerNull::get(builder_.getPtrTy());
    llvm::Constant* value = llvm::ConstantStruct::get(
        layout, {builder_.getInt64(sizeOf(type)), retain, release, builder_.getInt32(sema::isString(type) ? 1 : 0), builder_.getInt32(0)});
    auto global = new llvm::GlobalVariable(*module_, layout, true, llvm::GlobalValue::PrivateLinkage, value,
                                           "bob.element." + std::to_string(descriptors_.size()));
    descriptors_[type] = global;
    return global;
}

// ---------------------------------------------------------------------------
// optionals

llvm::Value* Generator::optionalPresent(llvm::Value* value, const Type* type)
{
    if (sema::isReference(type)) {
        return builder_.CreateICmpNE(value, llvm::ConstantPointerNull::get(builder_.getPtrTy()));
    }
    return builder_.CreateExtractValue(value, {0});
}

llvm::Value* Generator::optionalValue(llvm::Value* value, const Type* type)
{
    return sema::isReference(type) ? value : builder_.CreateExtractValue(value, {1});
}

llvm::Value* Generator::makeOptional(llvm::Value* present, llvm::Value* value, const Type* type)
{
    type = sema::canonical(type);
    if (sema::isReference(type)) {
        return builder_.CreateSelect(present, value, llvm::ConstantPointerNull::get(builder_.getPtrTy()));
    }
    // An absent optional always holds the zero value (so releasing it is harmless).
    llvm::Value* payload = builder_.CreateSelect(present, value, llvm::Constant::getNullValue(value->getType()));
    llvm::Value* result = llvm::UndefValue::get(llvmType(type));
    result = builder_.CreateInsertValue(result, present, {0});
    return builder_.CreateInsertValue(result, payload, {1});
}

llvm::Value* Generator::emitOptionalCompare(const BinarySend* send, const sema::SendInfo& info)
{
    bool leftNil = model_.typeOf(send->receiver)->kind == TypeKind::nilType;
    const Node* operand = leftNil ? send->argument : send->receiver;
    llvm::Value* value = emitAs(operand, model_.typeOf(operand));
    llvm::Value* present = optionalPresent(value, info.operandType);
    return info.op == "=" ? builder_.CreateNot(present) : present;
}

llvm::Value* Generator::emitInlinedIfNil(const KeywordSend* send, const sema::SendInfo& info)
{
    const Type* optional = sema::canonical(info.operandType);
    llvm::Value* value = emitAs(send->receiver, optional);
    llvm::Value* present = optionalPresent(value, optional);
    llvm::BasicBlock* nilBlock = llvm::BasicBlock::Create(context_, "ifnil", function_);
    llvm::BasicBlock* valueBlock = llvm::BasicBlock::Create(context_, "ifnotnil", function_);
    llvm::BasicBlock* merge = llvm::BasicBlock::Create(context_, "nilend", function_);
    builder_.CreateCondBr(present, valueBlock, nilBlock);

    const Type* resultType = info.resultType;
    bool wantsValue = resultType->kind != TypeKind::voidType;
    std::vector<std::string> parts;
    for (size_t start = 0; start < send->selector.size();) {
        size_t colon = send->selector.find(':', start);
        parts.push_back(send->selector.substr(start, colon - start + 1));
        start = colon + 1;
    }
    std::vector<std::pair<llvm::Value*, llvm::BasicBlock*>> incoming;
    auto branch = [&](llvm::BasicBlock* start, bool forNil) {
        builder_.SetInsertPoint(start);
        llvm::Value* result = nullptr;
        for (size_t i = 0; i < parts.size(); ++i) {
            if ((parts[i] == "ifNil:") != forNil) {
                continue;
            }
            auto literal = static_cast<const BlockLiteral*>(send->arguments[i]);
            if (!forNil) {
                const sema::LocalInfo* bound = model_.blockOf(literal)->params[0];
                llvm::Value* inner = optionalValue(value, optional);
                if (!isStackOnly(bound)) {
                    retainValue(inner, bound->type);
                }
                storeLocal(bound, inner);
            }
            size_t mark = temporaries_.size();
            result = wantsValue ? emitInlinedBody(literal, resultType) : nullptr;
            if (!wantsValue) {
                emitStatements(literal->statements);
            }
            flushTemporaries(mark);
        }
        if (builder_.GetInsertBlock()->getTerminator() == nullptr) {
            if (wantsValue) {
                incoming.push_back({result, builder_.GetInsertBlock()});
            }
            builder_.CreateBr(merge);
        }
    };
    branch(nilBlock, true);
    branch(valueBlock, false);
    builder_.SetInsertPoint(merge);
    if (!wantsValue) {
        return nullptr;
    }
    if (incoming.empty()) {
        return llvm::UndefValue::get(llvmType(resultType));
    }
    llvm::PHINode* phi = builder_.CreatePHI(llvmType(resultType), static_cast<unsigned>(incoming.size()));
    for (const auto& [v, from] : incoming) {
        phi->addIncoming(v, from);
    }
    return phi;
}

// ---------------------------------------------------------------------------
// unions: { tag: i32, payload: [n x i64] }, tag = index in the sorted members

llvm::StructType* Generator::unionType(const Type* type)
{
    auto it = unionTypes_.find(type);
    if (it != unionTypes_.end()) {
        return it->second;
    }
    uint64_t largest = 8;
    for (const Type* member : type->members) {
        largest = std::max(largest, sizeOf(member));
    }
    llvm::StructType* layout = llvm::StructType::create(
        context_, {builder_.getInt32Ty(), llvm::ArrayType::get(builder_.getInt64Ty(), (largest + 7) / 8)}, "union." + std::to_string(unionTypes_.size()));
    unionTypes_[type] = layout;
    return layout;
}

size_t Generator::unionIndex(const Type* type, const Type* member) const
{
    for (size_t i = 0; i < type->members.size(); ++i) {
        if (type->members[i] == sema::canonical(member)) {
            return i;
        }
    }
    return type->members.size();
}

llvm::Value* Generator::unionTag(llvm::Value* value)
{
    return builder_.CreateExtractValue(value, {0}, "tag");
}

llvm::Value* Generator::unionMember(llvm::Value* value, const Type* type, size_t member)
{
    llvm::Value* slot = spill(value, "union");
    llvm::Value* payload = builder_.CreateStructGEP(unionType(type), slot, 1);
    return builder_.CreateLoad(llvmType(type->members[member]), payload);
}

llvm::Value* Generator::unionWrap(llvm::Value* value, const Type* type, size_t member)
{
    llvm::StructType* layout = unionType(type);
    llvm::AllocaInst* slot = entryAlloca(layout, "union");
    builder_.CreateStore(llvm::Constant::getNullValue(layout), slot);
    builder_.CreateStore(builder_.getInt32(static_cast<uint32_t>(member)), builder_.CreateStructGEP(layout, slot, 0));
    builder_.CreateStore(value, builder_.CreateStructGEP(layout, slot, 1));
    return builder_.CreateLoad(layout, slot);
}

llvm::Value* Generator::switchOnUnion(llvm::Value* value, const Type* type, llvm::Type* resultType,
                                      const std::function<llvm::Value*(size_t, llvm::Value*)>& body)
{
    llvm::BasicBlock* merge = llvm::BasicBlock::Create(context_, "union.end", function_);
    llvm::BasicBlock* fallback = llvm::BasicBlock::Create(context_, "union.bad", function_);
    llvm::SwitchInst* dispatch = builder_.CreateSwitch(unionTag(value), fallback, static_cast<unsigned>(type->members.size()));
    std::vector<std::pair<llvm::Value*, llvm::BasicBlock*>> incoming;
    for (size_t i = 0; i < type->members.size(); ++i) {
        llvm::BasicBlock* arm = llvm::BasicBlock::Create(context_, "union.arm", function_);
        dispatch->addCase(builder_.getInt32(static_cast<uint32_t>(i)), arm);
        builder_.SetInsertPoint(arm);
        llvm::Value* member = unionMember(value, type, i);
        llvm::Value* result = body(i, member);
        incoming.push_back({result, builder_.GetInsertBlock()});
        builder_.CreateBr(merge);
    }
    builder_.SetInsertPoint(fallback);
    builder_.CreateUnreachable();
    builder_.SetInsertPoint(merge);
    if (resultType == nullptr || resultType->isVoidTy()) {
        return nullptr;
    }
    llvm::PHINode* phi = builder_.CreatePHI(resultType, static_cast<unsigned>(incoming.size()));
    for (const auto& [v, from] : incoming) {
        phi->addIncoming(v, from);
    }
    return phi;
}

llvm::Value* Generator::emitUnionSend(const Node* node, const sema::SendInfo& send)
{
    auto keyword = nodeCast<KeywordSend>(node);
    if (keyword == nullptr || (send.kind != SendKind::unionIsKindOf && send.kind != SendKind::unionIfKindOf)) {
        return nullptr;
    }
    const Type* type = model_.typeOf(keyword->receiver);
    llvm::Value* value = emitAs(keyword->receiver, type);
    size_t index = unionIndex(type, send.operandType);
    llvm::Value* matches = builder_.CreateICmpEQ(unionTag(value), builder_.getInt32(static_cast<uint32_t>(index)));
    if (send.kind == SendKind::unionIsKindOf) {
        return matches;
    }
    llvm::BasicBlock* body = llvm::BasicBlock::Create(context_, "kindof", function_);
    llvm::BasicBlock* done = llvm::BasicBlock::Create(context_, "kindof.end", function_);
    builder_.CreateCondBr(matches, body, done);
    builder_.SetInsertPoint(body);
    auto literal = static_cast<const BlockLiteral*>(keyword->arguments[1]);
    const sema::LocalInfo* bound = model_.blockOf(literal)->params[0];
    llvm::Value* member = coerce(unionMember(value, type, index), send.operandType, bound->type);
    retainValue(member, bound->type);
    storeReference(addressOf(bound), member, bound->type);
    size_t mark = temporaries_.size();
    emitStatements(literal->statements);
    flushTemporaries(mark);
    if (builder_.GetInsertBlock()->getTerminator() == nullptr) {
        builder_.CreateBr(done);
    }
    builder_.SetInsertPoint(done);
    return nullptr;
}

// ---------------------------------------------------------------------------
// conversions

llvm::Value* Generator::coerce(llvm::Value* value, const Type* from, const Type* to)
{
    from = sema::canonical(from);
    to = sema::canonical(to);
    if (from == to || to == nullptr || from == nullptr) {
        return value;
    }
    if (sema::isOptional(to)) {
        if (from->kind == TypeKind::nilType) {
            return defaultValue(to);
        }
        if (sema::isOptional(from) || sema::isReference(to)) {
            return value;
        }
        llvm::Value* inner = widen(value, from, to->inner);
        return makeOptional(builder_.getTrue(), inner, to);
    }
    if (sema::isUnion(to)) {
        if (sema::isUnion(from)) {
            return value;
        }
        size_t index = unionIndex(to, from);
        if (index == to->members.size()) {
            for (size_t i = 0; i < to->members.size(); ++i) {
                if (sema::canWiden(from, to->members[i])) {
                    index = i;
                    break;
                }
            }
            value = widen(value, from, to->members[index]);
        }
        return unionWrap(value, to, index);
    }
    if (sema::isSubrange(to)) {
        // Checked conversion into a subrange: trap when out of range.
        if (sema::isEnum(to->inner)) {
            llvm::Value* ordinal = enumOrdinal(value, to->enumInfo);
            llvm::Value* inRange = builder_.CreateAnd(builder_.CreateICmpSGE(ordinal, builder_.getInt32(static_cast<uint32_t>(to->low))),
                                                      builder_.CreateICmpSLE(ordinal, builder_.getInt32(static_cast<uint32_t>(to->high))));
            trapIf(builder_.CreateNot(inRange), abi::TrapReason::valueOutOfRange);
            return value;
        }
        const Type* source = sema::decay(from);
        llvm::Type* wide = builder_.getIntNTy(128);
        llvm::Value* extended = source->isSigned ? builder_.CreateSExt(value, wide) : builder_.CreateZExt(value, wide);
        llvm::Value* inRange = builder_.CreateAnd(
            builder_.CreateICmpSGE(extended, llvm::ConstantInt::get(wide, static_cast<uint64_t>(to->low), true)),
            builder_.CreateICmpSLE(extended, llvm::ConstantInt::get(wide, static_cast<uint64_t>(to->high), true)));
        trapIf(builder_.CreateNot(inRange), abi::TrapReason::valueOutOfRange);
        return builder_.CreateTrunc(extended, llvmType(to));
    }
    if (sema::isSubrange(from)) {
        return widen(value, from->inner, to);
    }
    return value;
}

// ---------------------------------------------------------------------------
// enums

llvm::GlobalVariable* Generator::enumValues(const sema::EnumInfo* info)
{
    auto it = enumValueTables_.find(info);
    if (it != enumValueTables_.end()) {
        return it->second;
    }
    llvm::Type* storage = llvmType(info->storage);
    std::vector<llvm::Constant*> values;
    for (const sema::EnumMemberInfo& member : info->members) {
        values.push_back(llvm::ConstantInt::get(storage, member.value));
    }
    llvm::ArrayType* type = llvm::ArrayType::get(storage, values.size());
    auto global = new llvm::GlobalVariable(*module_, type, true, llvm::GlobalValue::PrivateLinkage, llvm::ConstantArray::get(type, values),
                                           "bob." + info->name + ".values");
    enumValueTables_[info] = global;
    return global;
}

// `i32 find(storage value)`: the member's ordinal, or -1.
llvm::Function* Generator::enumFindFunction(const sema::EnumInfo* info)
{
    auto it = enumFinders_.find(info);
    if (it != enumFinders_.end()) {
        return it->second;
    }
    llvm::Type* storage = llvmType(info->storage);
    auto* function = llvm::Function::Create(llvm::FunctionType::get(builder_.getInt32Ty(), {storage}, false), llvm::GlobalValue::InternalLinkage,
                                            "bob." + info->name + ".find", module_.get());
    enumFinders_[info] = function;
    llvm::IRBuilder<> b(llvm::BasicBlock::Create(context_, "entry", function));
    llvm::BasicBlock* missing = llvm::BasicBlock::Create(context_, "missing", function);
    llvm::SwitchInst* dispatch = b.CreateSwitch(function->getArg(0), missing, static_cast<unsigned>(info->members.size()));
    for (const sema::EnumMemberInfo& member : info->members) {
        llvm::BasicBlock* found = llvm::BasicBlock::Create(context_, "found", function);
        dispatch->addCase(llvm::cast<llvm::ConstantInt>(llvm::ConstantInt::get(storage, member.value)), found);
        llvm::IRBuilder<>(found).CreateRet(b.getInt32(member.ordinal));
    }
    llvm::IRBuilder<>(missing).CreateRet(b.getInt32(static_cast<uint32_t>(-1)));
    return function;
}

llvm::Value* Generator::enumOrdinal(llvm::Value* value, const sema::EnumInfo* info)
{
    if (info->dense) {
        return builder_.CreateZExtOrTrunc(value, builder_.getInt32Ty());
    }
    return builder_.CreateCall(enumFindFunction(info), {value});
}

llvm::Value* Generator::enumMember(llvm::Value* ordinal, const sema::EnumInfo* info)
{
    llvm::Type* storage = llvmType(info->storage);
    if (info->dense) {
        return builder_.CreateZExtOrTrunc(ordinal, storage);
    }
    llvm::GlobalVariable* table = enumValues(info);
    llvm::Value* at = builder_.CreateGEP(table->getValueType(), table, {builder_.getInt32(0), ordinal});
    return builder_.CreateLoad(storage, at);
}

void Generator::emitOrdinalLoop(const sema::LocalInfo* local, const Type* elementType, llvm::Value* low, llvm::Value* high,
                                const std::vector<Node*>& body, llvm::Value* setMask)
{
    elementType = sema::canonical(elementType);
    llvm::Type* i64 = builder_.getInt64Ty();
    low = builder_.CreateSExtOrTrunc(low, i64);
    high = builder_.CreateSExtOrTrunc(high, i64);
    llvm::AllocaInst* counter = entryAlloca(i64, "ordinal");
    builder_.CreateStore(low, counter);

    llvm::BasicBlock* bodyBlock = llvm::BasicBlock::Create(context_, "each.body", function_);
    llvm::BasicBlock* run = llvm::BasicBlock::Create(context_, "each.run", function_);
    llvm::BasicBlock* step = llvm::BasicBlock::Create(context_, "each.step", function_);
    llvm::BasicBlock* increment = llvm::BasicBlock::Create(context_, "each.inc", function_);
    llvm::BasicBlock* exit = llvm::BasicBlock::Create(context_, "each.end", function_);
    builder_.CreateCondBr(builder_.CreateICmpSLE(low, high), bodyBlock, exit);

    builder_.SetInsertPoint(bodyBlock);
    llvm::Value* position = builder_.CreateLoad(i64, counter);
    llvm::Value* element = nullptr;
    if (sema::isOrdinal(elementType)) {
        element = enumMember(builder_.CreateTrunc(position, builder_.getInt32Ty()), elementType->enumInfo);
    }
    else {
        element = builder_.CreateTrunc(position, llvmType(elementType));
    }
    if (setMask != nullptr) {
        llvm::Value* bit = builder_.CreateShl(llvm::ConstantInt::get(setMask->getType(), 1), builder_.CreateZExtOrTrunc(element, setMask->getType()));
        builder_.CreateCondBr(builder_.CreateICmpNE(builder_.CreateAnd(setMask, bit), llvm::ConstantInt::get(setMask->getType(), 0)), run, step);
    }
    else {
        builder_.CreateBr(run);
    }
    builder_.SetInsertPoint(run);
    store(local, widen(element, elementType, local->type));
    loops_.push_back({exit, step});
    emitStatements(body);
    loops_.pop_back();
    if (builder_.GetInsertBlock()->getTerminator() == nullptr) {
        builder_.CreateBr(step);
    }

    builder_.SetInsertPoint(step);
    llvm::Value* current = builder_.CreateLoad(i64, counter);
    builder_.CreateCondBr(builder_.CreateICmpEQ(current, high), exit, increment);
    builder_.SetInsertPoint(increment);
    builder_.CreateStore(builder_.CreateAdd(current, llvm::ConstantInt::get(i64, 1)), counter);
    builder_.CreateBr(bodyBlock);
    builder_.SetInsertPoint(exit);
}

llvm::Value* Generator::emitEnumSend(const Node* node, const sema::SendInfo& send)
{
    auto unary = nodeCast<UnarySend>(node);
    auto keyword = nodeCast<KeywordSend>(node);
    const Type* type = sema::canonical(send.operandType);
    switch (send.kind) {
    case SendKind::constant:
        return llvm::ConstantInt::get(llvmType(send.resultType), send.constant);
    case SendKind::enumValue:
    case SendKind::distinctValue:
        return emitAs(unary->receiver, type);
    case SendKind::enumOrdinal:
        return enumOrdinal(emitAs(unary->receiver, type), send.enumInfo);
    case SendKind::enumSucc:
    case SendKind::enumPred: {
        bool succ = send.kind == SendKind::enumSucc;
        llvm::Value* ordinal = enumOrdinal(emitAs(unary->receiver, type), send.enumInfo);
        int64_t low = sema::isSubrange(type) ? type->low : 0;
        int64_t high = sema::isSubrange(type) ? type->high : static_cast<int64_t>(send.enumInfo->members.size()) - 1;
        trapIf(builder_.CreateICmpEQ(ordinal, builder_.getInt32(static_cast<uint32_t>(succ ? high : low))), abi::TrapReason::ordinalOutOfRange);
        return enumMember(builder_.CreateAdd(ordinal, builder_.getInt32(succ ? 1 : static_cast<uint32_t>(-1))), send.enumInfo);
    }
    case SendKind::enumFromValue: {
        llvm::Value* value = emitAs(keyword->arguments[0], send.enumInfo->storage);
        llvm::Value* ordinal = builder_.CreateCall(enumFindFunction(send.enumInfo), {value});
        int64_t low = sema::isSubrange(type) ? type->low : 0;
        int64_t high = sema::isSubrange(type) ? type->high : static_cast<int64_t>(send.enumInfo->members.size()) - 1;
        llvm::Value* ok = builder_.CreateAnd(builder_.CreateICmpSGE(ordinal, builder_.getInt32(static_cast<uint32_t>(low))),
                                             builder_.CreateICmpSLE(ordinal, builder_.getInt32(static_cast<uint32_t>(high))));
        return makeOptional(ok, value, send.resultType);
    }
    case SendKind::enumName: {
        const sema::EnumInfo* info = send.enumInfo;
        llvm::GlobalVariable*& table = enumNameTables_[info];
        if (table == nullptr) {
            std::vector<llvm::Constant*> names;
            for (const sema::EnumMemberInfo& member : info->members) {
                names.push_back(stringLiteral(member.name));
            }
            llvm::ArrayType* arrayType = llvm::ArrayType::get(builder_.getPtrTy(), names.size());
            table = new llvm::GlobalVariable(*module_, arrayType, true, llvm::GlobalValue::PrivateLinkage, llvm::ConstantArray::get(arrayType, names),
                                             "bob." + info->name + ".names");
        }
        llvm::Value* ordinal = enumOrdinal(emitAs(unary->receiver, type), info);
        // Names are immortal literals: the result needs no release, but follows the owned convention.
        return builder_.CreateLoad(builder_.getPtrTy(), builder_.CreateGEP(table->getValueType(), table, {builder_.getInt32(0), ordinal}));
    }
    case SendKind::distinctFromValue:
        return emitAs(keyword->arguments[0], type);
    case SendKind::subrangeFromValue: {
        const Type* target = sema::canonical(send.resultType)->inner;
        llvm::Value* value = emitAs(keyword->arguments[0], type);
        llvm::Type* wide = builder_.getIntNTy(128);
        llvm::Value* extended = type->isSigned ? builder_.CreateSExt(value, wide) : builder_.CreateZExt(value, wide);
        llvm::Value* ok = builder_.CreateAnd(builder_.CreateICmpSGE(extended, llvm::ConstantInt::get(wide, static_cast<uint64_t>(target->low), true)),
                                             builder_.CreateICmpSLE(extended, llvm::ConstantInt::get(wide, static_cast<uint64_t>(target->high), true)));
        return makeOptional(ok, builder_.CreateTrunc(extended, llvmType(target)), send.resultType);
    }
    case SendKind::inlineEnumDo: {
        auto literal = static_cast<const BlockLiteral*>(keyword->arguments[0]);
        bool whole = sema::isEnum(type);
        int64_t low = whole ? 0 : type->low;
        int64_t high = whole ? static_cast<int64_t>(type->enumInfo->members.size()) - 1 : type->high;
        emitOrdinalLoop(model_.blockOf(literal)->params[0], type, builder_.getInt64(static_cast<uint64_t>(low)),
                        builder_.getInt64(static_cast<uint64_t>(high)), literal->statements, nullptr);
        return nullptr;
    }
    default:
        return nullptr;
    }
}

// ---------------------------------------------------------------------------
// enum sets: bit n is the member whose value is n

llvm::Value* Generator::bitOf(llvm::Value* member, const Type* setType)
{
    llvm::Type* mask = llvmType(setType);
    return builder_.CreateShl(llvm::ConstantInt::get(mask, 1), builder_.CreateZExtOrTrunc(member, mask));
}

llvm::Value* Generator::emitBitSetSend(const Node* node, const sema::SendInfo& send)
{
    const Type* type = sema::canonical(send.operandType);
    llvm::Type* mask = type != nullptr && sema::isBitSet(type) ? llvmType(type) : nullptr;
    switch (send.kind) {
    case SendKind::bitSetLiteral: {
        llvm::Value* result = llvm::ConstantInt::get(mask, 0);
        for (const Node* element : static_cast<const ArrayLiteral*>(node)->elements) {
            result = builder_.CreateOr(result, bitOf(emitAs(element, type->inner), type));
        }
        return result;
    }
    case SendKind::bitSetOp: {
        auto binary = static_cast<const BinarySend*>(node);
        llvm::Value* a = emitAs(binary->receiver, type);
        llvm::Value* b = emitAs(binary->argument, type);
        if (send.op == "+") {
            return builder_.CreateOr(a, b);
        }
        if (send.op == "-") {
            return builder_.CreateAnd(a, builder_.CreateNot(b));
        }
        return builder_.CreateAnd(a, b);
    }
    case SendKind::bitSetIncludes: {
        auto keyword = static_cast<const KeywordSend*>(node);
        llvm::Value* set = emitAs(keyword->receiver, type);
        llvm::Value* bit = bitOf(emitAs(keyword->arguments[0], type->inner), type);
        return builder_.CreateICmpNE(builder_.CreateAnd(set, bit), llvm::ConstantInt::get(mask, 0));
    }
    case SendKind::bitSetCount: {
        llvm::Value* set = emitAs(static_cast<const UnarySend*>(node)->receiver, type);
        return builder_.CreateZExtOrTrunc(builder_.CreateUnaryIntrinsic(llvm::Intrinsic::ctpop, set), builder_.getInt32Ty());
    }
    case SendKind::bitSetValue:
        return emitAs(static_cast<const UnarySend*>(node)->receiver, type);
    case SendKind::bitSetIsEmpty:
        return builder_.CreateICmpEQ(emitAs(static_cast<const UnarySend*>(node)->receiver, type), llvm::ConstantInt::get(mask, 0));
    case SendKind::bitSetFromValue: {
        auto keyword = static_cast<const KeywordSend*>(node);
        llvm::Value* value = emitAs(keyword->arguments[0], model_.typeOf(keyword->arguments[0]));
        value = builder_.CreateZExtOrTrunc(value, mask);
        uint64_t valid = 0;
        for (const sema::EnumMemberInfo& member : type->inner->enumInfo->members) {
            valid |= uint64_t(1) << member.value;
        }
        llvm::Value* ok = builder_.CreateICmpEQ(builder_.CreateAnd(value, llvm::ConstantInt::get(mask, ~valid)), llvm::ConstantInt::get(mask, 0));
        return makeOptional(ok, value, send.resultType);
    }
    case SendKind::bitSetMutate: {
        auto keyword = static_cast<const KeywordSend*>(node);
        llvm::Value* address = emitAddress(keyword->receiver);
        llvm::Value* bit = bitOf(emitAs(keyword->arguments[0], type->inner), type);
        llvm::Value* set = builder_.CreateLoad(mask, address);
        builder_.CreateStore(send.op == "add:" ? builder_.CreateOr(set, bit) : builder_.CreateAnd(set, builder_.CreateNot(bit)), address);
        return nullptr;
    }
    default:
        return nullptr;
    }
}

}
