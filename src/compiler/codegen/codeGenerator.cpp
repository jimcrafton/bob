#include "codegenInternal.h"

#include <algorithm>
#include <cmath>

namespace bob::codegen::detail {

// ---------------------------------------------------------------------------
// module structure

std::unique_ptr<llvm::Module> Generator::run(const Unit* unit)
{
    module_ = std::make_unique<llvm::Module>(unit->name.empty() ? "bob" : unit->name, context_);
    module_->setDataLayout(dataLayout_);
    if (!triple_.empty()) {
        module_->setTargetTriple(llvm::Triple(triple_));
    }
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
        // @[compiletime] messages only run inside evaluation modules (and the
        // synthesized contexts of compile-time items only as their thunk).
        bool exported = message->origin.exported || (message->owner != nullptr && message->owner->origin.exported);
        bool skip = isCompileTimeContext(message.get()) || (message->compileTimeOnly && evaluating_ == nullptr && !exported);
        if (!isInitializer && !message->isRequired && !skip) {
            declareMessage(message.get());
        }
    }
    for (const auto& message : model_.messages) {
        bool imported = message->imported || (message->owner != nullptr && message->owner->origin.module != nullptr);
        if (functions_.count(message.get()) != 0 && !imported) {
            emitMessage(message.get());
        }
    }
    for (const auto& info : model_.classes) {
        emitClass(info.get());
    }
    if (evaluating_ != nullptr) {
        emitThunk();
    }
    else if (model_.program.main != nullptr) {
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
    return llvm::StructType::get(context_, {ptr, ptr, builder_.getInt64Ty(), ptr, ptr, builder_.getInt32Ty(), builder_.getInt32Ty(), ptr, ptr,
                                            llvm::ArrayType::get(ptr, vtableSize)});
}

// A protocol's descriptor (BobProtocol): one link-once copy program-wide, so
// its address identifies the protocol across modules, as selectors do.
llvm::GlobalVariable* Generator::protocolGlobal(const sema::ClassInfo* protocol)
{
    auto it = protocolGlobals_.find(protocol);
    if (it != protocolGlobals_.end()) {
        return it->second;
    }
    std::string name = "bob.protocol." + protocol->unitName + "." + protocol->name;
    llvm::Constant* text = llvm::ConstantDataArray::getString(context_, protocol->name);
    auto nameGlobal = new llvm::GlobalVariable(*module_, text->getType(), true, llvm::GlobalValue::LinkOnceODRLinkage, text, name + ".name");
    nameGlobal->setComdat(module_->getOrInsertComdat(name + ".name"));
    llvm::StructType* type = llvm::StructType::get(context_, {builder_.getPtrTy(), builder_.getInt64Ty()});
    llvm::Constant* value = llvm::ConstantStruct::get(type, {nameGlobal, builder_.getInt64(protocol->witnessSelectors().size())});
    auto global = new llvm::GlobalVariable(*module_, type, true, llvm::GlobalValue::LinkOnceODRLinkage, value, name);
    global->setComdat(module_->getOrInsertComdat(name));
    protocolGlobals_[protocol] = global;
    return global;
}

// `bob.witness(class, protocol)`, inlined at each protocol send. A class that
// receives a protocol send conforms to at least one protocol, so its first
// conformance entry can always be read: that's checked inline, with loads of
// memory that never changes, so a loop over one receiver looks its slot up
// once. Otherwise the runtime's bobWitness scans the list (its result depends
// only on its arguments, so it can be hoisted too) and traps if nothing matches.
llvm::Function* Generator::witnessFunction()
{
    if (llvm::Function* existing = module_->getFunction("bob.witness")) {
        return existing;
    }
    llvm::Type* ptr = builder_.getPtrTy();
    llvm::FunctionType* type = llvm::FunctionType::get(ptr, {ptr, ptr}, false);
    llvm::Function* scan = llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage, "bobWitness", module_.get());
    scan->setDoesNotAccessMemory();
    scan->setDoesNotThrow();
    scan->setWillReturn();
    llvm::Function* function = llvm::Function::Create(type, llvm::GlobalValue::LinkOnceODRLinkage, "bob.witness", module_.get());
    function->setComdat(module_->getOrInsertComdat("bob.witness"));
    function->addFnAttr(llvm::Attribute::AlwaysInline);
    function->setDoesNotThrow();

    llvm::IRBuilderBase::InsertPoint savedPoint = builder_.saveIP();
    llvm::BasicBlock* entry = llvm::BasicBlock::Create(context_, "entry", function);
    llvm::BasicBlock* first = llvm::BasicBlock::Create(context_, "first", function);
    llvm::BasicBlock* other = llvm::BasicBlock::Create(context_, "scan", function);
    llvm::MDNode* invariant = llvm::MDNode::get(context_, {});
    auto invariantLoad = [&](llvm::Type* loaded, llvm::Value* address) {
        llvm::LoadInst* load = builder_.CreateLoad(loaded, address);
        load->setMetadata(llvm::LLVMContext::MD_invariant_load, invariant);
        return load;
    };
    llvm::Value* cls = function->getArg(0);
    llvm::Value* protocol = function->getArg(1);

    builder_.SetInsertPoint(entry);
    llvm::Value* list = invariantLoad(ptr, builder_.CreateConstInBoundsGEP1_64(builder_.getInt8Ty(), cls, 56));
    llvm::Value* candidate = invariantLoad(ptr, list);
    llvm::Value* witness = invariantLoad(ptr, builder_.CreateConstInBoundsGEP1_64(builder_.getInt8Ty(), list, 8));
    builder_.CreateCondBr(builder_.CreateICmpEQ(candidate, protocol), first, other);

    builder_.SetInsertPoint(first);
    builder_.CreateRet(witness);

    builder_.SetInsertPoint(other);
    builder_.CreateRet(builder_.CreateCall(scan, {cls, protocol}));
    builder_.restoreIP(savedPoint);
    return function;
}

// The class's conformance list: a witness table for each protocol it conforms
// to, in that protocol's slot order. Returns {list, count}.
std::pair<llvm::Constant*, uint32_t> Generator::emitConformances(const sema::ClassInfo* info)
{
    llvm::Type* ptr = builder_.getPtrTy();
    llvm::StructType* entryType = llvm::StructType::get(context_, {ptr, ptr});
    std::vector<llvm::Constant*> entries;
    for (const sema::ClassInfo* protocol : info->conformedProtocols()) {
        std::vector<llvm::Constant*> slots;
        for (const std::string& selector : protocol->witnessSelectors()) {
            const sema::MessageInfo* implementation = info->lookup(selector);
            auto function = implementation != nullptr ? functions_.find(implementation) : functions_.end();
            slots.push_back(function != functions_.end() ? static_cast<llvm::Constant*>(function->second) : llvm::ConstantPointerNull::get(builder_.getPtrTy()));
        }
        llvm::ArrayType* tableType = llvm::ArrayType::get(ptr, slots.size());
        auto table = new llvm::GlobalVariable(*module_, tableType, true, llvm::GlobalValue::PrivateLinkage, llvm::ConstantArray::get(tableType, slots),
                                              "witness." + info->unitName + "." + info->name + "." + protocol->name);
        entries.push_back(llvm::ConstantStruct::get(entryType, {protocolGlobal(protocol), table}));
    }
    if (entries.empty()) {
        return {llvm::ConstantPointerNull::get(builder_.getPtrTy()), 0};
    }
    llvm::ArrayType* listType = llvm::ArrayType::get(entryType, entries.size());
    auto list = new llvm::GlobalVariable(*module_, listType, true, llvm::GlobalValue::PrivateLinkage, llvm::ConstantArray::get(listType, entries),
                                         "conformances." + info->unitName + "." + info->name);
    return {list, static_cast<uint32_t>(entries.size())};
}

// Selectors are unique strings; bob_msgSend compares them by address.
llvm::GlobalVariable* Generator::selectorGlobal(const std::string& selector)
{
    auto it = selectors_.find(selector);
    if (it != selectors_.end()) {
        return it->second;
    }
    llvm::Constant* text = llvm::ConstantDataArray::getString(context_, selector);
    // One copy program-wide: modules each emit it, and the linker keeps one (COMDAT),
    // so selectors compare by address across module boundaries.
    std::string name = "bob.sel." + selector;
    auto global = new llvm::GlobalVariable(*module_, text->getType(), true, llvm::GlobalValue::LinkOnceODRLinkage, text, name);
    global->setComdat(module_->getOrInsertComdat(name));
    selectors_[selector] = global;
    return global;
}

// bob_msgSend (reflection) calls an implementation as (self, selector, args...),
// keeping the selector in its register; compiled messages take (self, args...).
// This adapter drops the selector. Only the method table refers to it.
llvm::Function* Generator::reflectionThunk(const sema::MessageInfo* message)
{
    auto it = reflectionThunks_.find(message);
    if (it != reflectionThunks_.end()) {
        return it->second;
    }
    llvm::Function* target = functions_.at(message);
    llvm::FunctionType* targetType = target->getFunctionType();
    std::vector<llvm::Type*> params{builder_.getPtrTy(), builder_.getPtrTy()};
    params.insert(params.end(), targetType->param_begin() + 1, targetType->param_end());
    llvm::Function* thunk = llvm::Function::Create(llvm::FunctionType::get(targetType->getReturnType(), params, false),
                                                   llvm::GlobalValue::InternalLinkage, message->symbolName + ".msgSend", module_.get());
    llvm::IRBuilderBase::InsertPoint savedPoint = builder_.saveIP();
    builder_.SetInsertPoint(llvm::BasicBlock::Create(context_, "entry", thunk));
    std::vector<llvm::Value*> args{thunk->getArg(0)};
    for (unsigned i = 2; i < thunk->arg_size(); ++i) {
        args.push_back(thunk->getArg(i));
    }
    llvm::CallInst* call = builder_.CreateCall(target, args);
    call->setTailCall();
    if (targetType->getReturnType()->isVoidTy()) {
        builder_.CreateRetVoid();
    }
    else {
        builder_.CreateRet(call);
    }
    builder_.restoreIP(savedPoint);
    reflectionThunks_[message] = thunk;
    return thunk;
}

// Instance messages take (self, params...); class-side messages only their params.
llvm::FunctionType* Generator::methodType(const sema::MessageInfo* message)
{
    std::vector<llvm::Type*> params;
    if (message->isInstance) {
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
    // An exported class is defined here for importers; an imported one is defined
    // by its module (design §11.5). Symbols carry the declaring unit's name.
    bool imported = info->origin.module != nullptr;
    auto linkage = imported || info->origin.exported ? llvm::GlobalValue::ExternalLinkage : llvm::GlobalValue::InternalLinkage;
    std::string prefix = info->unitName + "." + info->name;
    classGlobals_[info] = new llvm::GlobalVariable(*module_, classInfoType(info->vtable.size()), false, linkage, nullptr, "class." + prefix);
    destroyFunctions_[info] = llvm::Function::Create(llvm::FunctionType::get(builder_.getVoidTy(), {ptr}, false), linkage,
                                                     "bob." + prefix + ".destroy", module_.get());
    initFunctions_[info] = llvm::Function::Create(llvm::FunctionType::get(builder_.getVoidTy(), {ptr, builder_.getInt64Ty()}, false), linkage,
                                                  "bob." + prefix + ".fields", module_.get());
    initsFunctions_[info] = llvm::Function::Create(llvm::FunctionType::get(builder_.getVoidTy(), {ptr}, false), linkage,
                                                   "bob." + prefix + ".inits", module_.get());
    newFunctions_[info] = llvm::Function::Create(llvm::FunctionType::get(ptr, {}, false), linkage, "bob." + prefix + ".new", module_.get());
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
    if (info->isProtocol || info->origin.module != nullptr) {
        return;
    }
    llvm::Type* ptr = builder_.getPtrTy();

    // destroy: release the object's reference fields
    function_ = destroyFunctions_.at(info);
    builder_.SetInsertPoint(llvm::BasicBlock::Create(context_, "entry", function_));
    for (const sema::FieldInfo& field : info->fields) {
        if (sema::needsCleanup(field.type)) {
            dropHeld(builder_.CreateLoad(llvmType(field.type), fieldAddress(function_->getArg(0), &field)), field.type, field.ownership);
        }
    }
    builder_.CreateRetVoid();

    // fields(object, passed): the field initializers, superclass first, skipping
    // the fields a constructor message passed (bit n of `passed` = field n).
    beginFunction(initFunctions_.at(info), info->initializer, info->initializer->locals);
    llvm::Value* object = function_->getArg(0);
    llvm::Value* passed = function_->getArg(1);
    if (info->superclass != nullptr) {
        builder_.CreateCall(initFunctions_.at(info->superclass), {object, passed});
    }
    for (const sema::FieldInfo& field : info->fields) {
        if (field.owner != info) {
            continue;
        }
        llvm::Constant* initial = defaultValue(field.type);
        // Objects start zero-filled, so a field defaulting to zero needs no code.
        if (field.initializer == nullptr && !field.constructsDefault && initial->isNullValue()) {
            continue;
        }
        llvm::BasicBlock* set = llvm::BasicBlock::Create(context_, "field." + field.name, function_);
        llvm::BasicBlock* next = llvm::BasicBlock::Create(context_, "field.next", function_);
        llvm::Value* bit = builder_.CreateAnd(passed, builder_.getInt64(uint64_t(1) << field.index));
        builder_.CreateCondBr(builder_.CreateICmpEQ(bit, builder_.getInt64(0)), set, next);
        builder_.SetInsertPoint(set);
        size_t mark = temporaries_.size();
        llvm::Value* value = initial;
        if (field.initializer != nullptr) {
            value = emitOwnedAs(field.initializer, field.type);
        }
        else if (field.constructsDefault) {
            value = builder_.CreateCall(newFunctions_.at(field.type->classInfo), {}, field.name);
        }
        initializeHeld(fieldAddress(object, &field), value, field.type, field.ownership);
        flushTemporaries(mark);
        builder_.CreateBr(next);
        builder_.SetInsertPoint(next);
    }
    emitReturn(nullptr);

    // inits(object): each class's own `init`, superclass first. When it returns the object is complete.
    function_ = initsFunctions_.at(info);
    builder_.SetInsertPoint(llvm::BasicBlock::Create(context_, "entry", function_));
    if (info->superclass != nullptr) {
        builder_.CreateCall(initsFunctions_.at(info->superclass), {function_->getArg(0)});
    }
    if (info->initMessage != nullptr) {
        builder_.CreateCall(functions_.at(info->initMessage), {function_->getArg(0)});
    }
    builder_.CreateRetVoid();

    // new: the default constructor behind `x: C;` and default-constructed fields.
    function_ = newFunctions_.at(info);
    builder_.SetInsertPoint(llvm::BasicBlock::Create(context_, "entry", function_));
    llvm::Value* created = builder_.CreateCall(allocFunction_, {classGlobals_.at(info)}, "object");
    builder_.CreateCall(initFunctions_.at(info), {created, builder_.getInt64(0)});
    builder_.CreateCall(initsFunctions_.at(info), {created});
    builder_.CreateRet(created);

    // class info: header, method table for bob_msgSend (reflection), vtable
    std::vector<llvm::Constant*> entries;
    llvm::StructType* entryType = llvm::StructType::get(context_, {ptr, ptr});
    for (const sema::MessageInfo* method : info->methodList) {
        if (functions_.count(method) != 0) {
            entries.push_back(llvm::ConstantStruct::get(entryType, {selectorGlobal(method->selector), reflectionThunk(method)}));
        }
    }
    llvm::ArrayType* tableType = llvm::ArrayType::get(entryType, entries.size());
    auto table = new llvm::GlobalVariable(*module_, tableType, true, llvm::GlobalValue::PrivateLinkage,
                                          llvm::ConstantArray::get(tableType, entries), "methods." + info->unitName + "." + info->name);
    std::vector<llvm::Constant*> vtable;
    for (const sema::MessageInfo* method : info->vtable) {
        auto function = method != nullptr ? functions_.find(method) : functions_.end();
        vtable.push_back(function != functions_.end() ? static_cast<llvm::Constant*>(function->second)
                                                      : llvm::ConstantPointerNull::get(builder_.getPtrTy()));
    }
    llvm::StructType* type = classInfoType(info->vtable.size());
    llvm::Constant* name = builder_.CreateGlobalString(info->name, "name." + info->unitName + "." + info->name, 0, module_.get());
    uint64_t size = module_->getDataLayout().getTypeAllocSize(objectType(info));
    llvm::Constant* superclass = info->superclass != nullptr ? static_cast<llvm::Constant*>(classGlobals_.at(info->superclass))
                                                             : llvm::ConstantPointerNull::get(builder_.getPtrTy());
    auto [conformances, conformanceCount] = emitConformances(info);
    classGlobals_.at(info)->setInitializer(llvm::ConstantStruct::get(
        type, {superclass, name, builder_.getInt64(size), destroyFunctions_.at(info), table, builder_.getInt32(static_cast<uint32_t>(entries.size())),
               builder_.getInt32(conformanceCount), llvm::ConstantPointerNull::get(builder_.getPtrTy()), conformances,
               llvm::ConstantArray::get(llvm::ArrayType::get(ptr, vtable.size()), vtable)}));
}

// `Point x: 4 y: 7`: evaluate the arguments, allocate, store the passed fields,
// then run the other fields' initializers and each class's `init`.
llvm::Value* Generator::emitConstruct(const KeywordSend* send, const sema::SendInfo& info, llvm::Value* place)
{
    std::vector<llvm::Value*> values;
    uint64_t passed = 0;
    for (size_t i = 0; i < send->arguments.size(); ++i) {
        values.push_back(emitOwnedAs(send->arguments[i], info.fields[i]->type));
        passed |= uint64_t(1) << info.fields[i]->index;
    }
    llvm::Value* object = place;
    if (object == nullptr) {
        object = builder_.CreateCall(allocFunction_, {classGlobals_.at(info.classInfo)}, "object");
    }
    else {
        initializeObjectHeader(object, info.classInfo);
    }
    for (size_t i = 0; i < values.size(); ++i) {
        initializeHeld(fieldAddress(object, info.fields[i]), values[i], info.fields[i]->type, info.fields[i]->ownership);
    }
    builder_.CreateCall(initFunctions_.at(info.classInfo), {object, builder_.getInt64(passed)});
    builder_.CreateCall(initsFunctions_.at(info.classInfo), {object});
    return object;
}

void Generator::allocateStackObjects(const sema::MessageInfo* message, const sema::BlockInfo* block)
{
    for (const auto& [id, object] : model_.stackObjects) {
        if (object.block != block || (block == nullptr && object.message != message)) {
            continue;
        }
        StackSlot slot;
        slot.object = entryAlloca(objectType(object.classInfo), object.local->name + ".stack");
        slot.object->setAlignment(llvm::Align(16));
        slot.built = entryAlloca(builder_.getInt1Ty(), object.local->name + ".built");
        slot.classInfo = object.classInfo;
        builder_.CreateStore(builder_.getFalse(), slot.built);
        stackSlots_[id] = slot;
    }
}

// A stack object's header: its class, and the immortal flag so retain/release
// on it do nothing. If the slot already holds an object from an earlier pass of
// a loop, that object's fields are released first.
void Generator::initializeObjectHeader(llvm::Value* object, const sema::ClassInfo* info)
{
    for (const auto& [id, slot] : stackSlots_) {
        if (slot.object != object) {
            continue;
        }
        llvm::BasicBlock* drop = llvm::BasicBlock::Create(context_, "stack.drop", function_);
        llvm::BasicBlock* build = llvm::BasicBlock::Create(context_, "stack.build", function_);
        builder_.CreateCondBr(builder_.CreateLoad(builder_.getInt1Ty(), slot.built), drop, build);
        builder_.SetInsertPoint(drop);
        builder_.CreateCall(destroyFunctions_.at(info), {object});
        builder_.CreateBr(build);
        builder_.SetInsertPoint(build);
    }
    uint64_t size = module_->getDataLayout().getTypeAllocSize(objectType(info));
    builder_.CreateMemSet(object, builder_.getInt8(0), size, llvm::MaybeAlign(16));
    builder_.CreateStore(classGlobals_.at(info), object);
    builder_.CreateStore(builder_.getInt32(1), builder_.CreateConstInBoundsGEP1_64(builder_.getInt8Ty(), object, 8));
    builder_.CreateStore(builder_.getInt32(bobObjectImmortal | bobObjectStack), builder_.CreateConstInBoundsGEP1_64(builder_.getInt8Ty(), object, 12));
}

llvm::Value* Generator::emitStackObject(const Node* statement, const sema::StackObject& object)
{
    const StackSlot& slot = stackSlots_.at(statement->id);
    llvm::Value* place = slot.object;
    const sema::SendInfo* send = model_.sendOf(object.allocation);
    if (send != nullptr && send->kind == SendKind::constructObject) {
        emitConstruct(static_cast<const KeywordSend*>(object.allocation), *send, place);
    }
    else {
        initializeObjectHeader(place, object.classInfo);
        builder_.CreateCall(initFunctions_.at(object.classInfo), {place, builder_.getInt64(0)});
        builder_.CreateCall(initsFunctions_.at(object.classInfo), {place});
    }
    builder_.CreateStore(builder_.getTrue(), slot.built);
    return place;
}

void Generator::destroyStackObjects()
{
    for (const auto& [id, slot] : stackSlots_) {
        llvm::BasicBlock* drop = llvm::BasicBlock::Create(context_, "stack.release", function_);
        llvm::BasicBlock* done = llvm::BasicBlock::Create(context_, "stack.done", function_);
        builder_.CreateCondBr(builder_.CreateLoad(builder_.getInt1Ty(), slot.built), drop, done);
        builder_.SetInsertPoint(drop);
        builder_.CreateCall(destroyFunctions_.at(slot.classInfo), {slot.object});
        builder_.CreateBr(done);
        builder_.SetInsertPoint(done);
    }
}

// The generated `copy`: a new object of the same class with the same field
// values (references shared, so each one is retained), then `init`.
void Generator::emitCopy(const sema::MessageInfo* message)
{
    const sema::ClassInfo* info = message->owner;
    function_ = functions_.at(message);
    builder_.SetInsertPoint(llvm::BasicBlock::Create(context_, "entry", function_));
    llvm::Value* self = function_->getArg(0);
    llvm::Value* copy = builder_.CreateCall(allocFunction_, {classGlobals_.at(info)}, "copy");
    uint64_t size = module_->getDataLayout().getTypeAllocSize(objectType(info));
    if (size > abi::objectHeaderSize) {
        builder_.CreateMemCpy(builder_.CreateConstInBoundsGEP1_64(builder_.getInt8Ty(), copy, abi::objectHeaderSize), llvm::MaybeAlign(8),
                              builder_.CreateConstInBoundsGEP1_64(builder_.getInt8Ty(), self, abi::objectHeaderSize), llvm::MaybeAlign(8),
                              size - abi::objectHeaderSize);
    }
    for (const sema::FieldInfo& field : info->fields) {
        if (sema::needsCleanup(field.type)) {
            copyHeld(builder_.CreateLoad(llvmType(field.type), fieldAddress(copy, &field)), field.type, field.ownership);
        }
    }
    builder_.CreateCall(initsFunctions_.at(info), {copy});
    builder_.CreateRet(copy);
}

// ---------------------------------------------------------------------------
// reference counting: variables and fields own their reference; parameters are
// borrowed; call results are owned. Owned values used only for the duration
// of a statement are "temporaries", released when the statement ends.

bool Generator::producesOwned(const Node* node) const
{
    if (!sema::needsCleanup(model_.typeOf(node))) {
        return false;
    }
    // Embedded compile-time values are immortal constants or built fresh (+1).
    if (model_.compileTimeNodes.count(node->id) != 0 || model_.constantRefs.count(node->id) != 0) {
        return true;
    }
    if (node->kind == NodeKind::identifier) {
        if (model_.selfFieldRefs.count(node->id) != 0) {
            return true;
        }
        const sema::LocalInfo* local = model_.localOf(node);
        return local != nullptr && ((local->captured && !isStackOnly(local)) || local->ownership != sema::Ownership::strong);
    }
    if (node->kind == NodeKind::selfExpr) {
        const sema::LocalInfo* self = model_.localOf(node);
        return self != nullptr && self->captured;
    }
    if (node->kind == NodeKind::fieldAccess || node->kind == NodeKind::blockLiteral) {
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
    if (sema::needsCleanup(model_.typeOf(expression)) && !producesOwned(expression) && !isStackOnlyValue(expression)) {
        retainValue(value, model_.typeOf(expression));
    }
    return widen(value, model_.typeOf(expression), target);
}

void Generator::storeReference(llvm::Value* address, llvm::Value* owned, const Type* type)
{
    if (!sema::needsCleanup(type)) {
        builder_.CreateStore(owned, address);
        return;
    }
    llvm::Value* old = builder_.CreateLoad(llvmType(type), address, "old");
    builder_.CreateStore(owned, address);
    releaseValue(old, type);
}

bool Generator::isStackOnly(const sema::LocalInfo* local) const
{
    return local != nullptr && model_.stackOnlyLocals.count(local) != 0;
}

bool Generator::isStackOnlyValue(const Node* expression) const
{
    while (auto attributed = nodeCast<AttributedExpr>(expression)) {
        expression = attributed->expression;
    }
    if (expression == nullptr || expression->kind != NodeKind::identifier || model_.selfFieldRefs.count(expression->id) != 0) {
        return false;
    }
    return isStackOnly(model_.localOf(expression));
}

void Generator::storeLocal(const sema::LocalInfo* local, llvm::Value* owned)
{
    if (isStackOnly(local)) {
        builder_.CreateStore(owned, addressOf(local));
        return;
    }
    storeHeld(addressOf(local), owned, local->type, local->ownership);
}

void Generator::flushTemporaries(size_t mark)
{
    if (blockTerminated()) {
        temporaries_.resize(std::min(mark, temporaries_.size()));
        return;
    }
    while (temporaries_.size() > mark) {
        releaseValue(temporaries_.back().value, temporaries_.back().type);
        temporaries_.pop_back();
    }
}

void Generator::releaseLocals()
{
    for (const auto& [local, slot] : slots_) {
        if (isBoxed(local)) {
            // The box releases the value when the last closure sharing it lets go.
            builder_.CreateCall(releaseFunction_, {builder_.CreateLoad(builder_.getPtrTy(), slot)});
            continue;
        }
        if (sema::needsCleanup(local->type) && !local->isParameter && !isStackOnly(local)) {
            dropHeld(builder_.CreateLoad(llvmType(local->type), slot), local->type, local->ownership);
        }
    }
    destroyStackObjects();
}

// Every exit from a function: release pending temporaries (they stay listed for
// other paths) and owned locals, then return.
void Generator::emitReturn(llvm::Value* value)
{
    for (auto it = temporaries_.rbegin(); it != temporaries_.rend(); ++it) {
        releaseValue(it->value, it->type);
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
    createSlots(locals);
    stackSlots_.clear();
    allocateStackObjects(message, nullptr);
}

llvm::Type* Generator::llvmType(const Type* type)
{
    type = sema::canonical(type);
    switch (type->kind) {
    case TypeKind::enumType:
    case TypeKind::subrange:
    case TypeKind::character:
        return builder_.getIntNTy(type->bits);
    case TypeKind::set:
        return sema::isBitSet(type) ? static_cast<llvm::Type*>(builder_.getIntNTy(type->bits)) : builder_.getPtrTy();
    case TypeKind::string:
    case TypeKind::stringView:
    case TypeKind::array:
    case TypeKind::dictionary:
    case TypeKind::hashtable:
        return builder_.getPtrTy();
    case TypeKind::fixedArray:
        return llvm::ArrayType::get(llvmType(type->inner), type->count);
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
    case TypeKind::optional:
        if (sema::isValueOptional(type)) {
            return llvm::StructType::get(context_, {builder_.getInt1Ty(), llvmType(type->inner)});
        }
        return builder_.getPtrTy();
    case TypeKind::classType:
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
    bool imported = message->imported || (message->owner != nullptr && message->owner->origin.module != nullptr);
    bool exported = message->origin.exported || (message->owner != nullptr && message->owner->origin.exported);
    auto linkage = imported || exported ? llvm::GlobalValue::ExternalLinkage : llvm::GlobalValue::InternalLinkage;
    llvm::Function* function = llvm::Function::Create(methodType(message), linkage, message->symbolName, module_.get());
    unsigned offset = message->isInstance ? 1 : 0;
    if (message->isInstance) {
        function->getArg(0)->setName("self");
    }
    for (size_t i = 0; i < message->params.size(); ++i) {
        function->getArg(static_cast<unsigned>(i) + offset)->setName(message->params[i]->name);
    }
    functions_[message] = function;
}

void Generator::emitMessage(const sema::MessageInfo* message)
{
    if (message->isCopy) {
        emitCopy(message);
        return;
    }
    beginFunction(functions_.at(message), message, message->locals);
    unsigned offset = 0;
    if (message->isInstance) {
        builder_.CreateStore(function_->getArg(0), slots_.at(message->self));
        offset = 1;
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
    if (!blockTerminated()) {
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
    // bobMain(args): the runtime passes the command-line arguments as an array of strings (borrowed).
    auto type = llvm::FunctionType::get(builder_.getInt32Ty(), {builder_.getPtrTy()}, false);
    llvm::Function* entry = llvm::Function::Create(type, llvm::GlobalValue::ExternalLinkage, abi::mainSymbol, module_.get());
    builder_.SetInsertPoint(llvm::BasicBlock::Create(context_, "entry", entry));
    std::vector<llvm::Value*> args;
    for (size_t i = 0; i < model_.program.main->params.size(); ++i) {
        args.push_back(entry->getArg(0));
    }
    builder_.CreateRet(builder_.CreateCall(functions_.at(model_.program.main), args));
}

// ---------------------------------------------------------------------------
// statements

void Generator::ensureLiveBlock()
{
    if (blockTerminated()) {
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

// A variable is either owned by the current function (an alloca, or a box the
// alloca points to) or captured from an enclosing one, in which case the
// block's environment holds its address or, in a heap closure, its value.
llvm::Value* Generator::addressOf(const sema::LocalInfo* local)
{
    auto slot = slots_.find(local);
    if (slot != slots_.end()) {
        if (isBoxed(local)) {
            llvm::Value* box = builder_.CreateLoad(builder_.getPtrTy(), slot->second, local->name + ".box");
            return builder_.CreateConstInBoundsGEP1_64(builder_.getInt8Ty(), box, abi::objectHeaderSize, local->name + ".boxed");
        }
        return slot->second;
    }
    const auto& captures = block_->captures;
    auto it = std::find(captures.begin(), captures.end(), local);
    unsigned index = static_cast<unsigned>(it - captures.begin());
    llvm::Value* field = builder_.CreateStructGEP(environmentType(block_), environment_, index + 3);
    if (capturedByValue(block_, local)) {
        return field;
    }
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
        llvm::Value* value = nullptr;
        if (auto stack = model_.stackObjects.find(decl->id); stack != model_.stackObjects.end()) {
            value = emitStackObject(decl, stack->second);
        }
        else if (decl->initializer != nullptr) {
            value = emitOwnedAs(decl->initializer, local->type);
        }
        else if (auto object = model_.defaultObjects.find(decl->id); object != model_.defaultObjects.end()) {
            value = builder_.CreateCall(newFunctions_.at(object->second), {}, decl->name);
        }
        else {
            value = defaultValue(local->type);
        }
        // Each pass of a loop declares a new variable, so closures made in
        // different passes don't share it.
        if (isBoxed(local) && !loops_.empty()) {
            allocateBox(local);
        }
        storeLocal(local, value);
        break;
    }
    case NodeKind::assignment: {
        auto assignment = static_cast<const Assignment*>(statement);
        const sema::LocalInfo* local = model_.localOf(assignment->target);
        if (local != nullptr) {
            auto stack = model_.stackObjects.find(assignment->id);
            llvm::Value* value = stack != model_.stackObjects.end() ? emitStackObject(assignment, stack->second)
                                                                    : emitOwnedAs(assignment->value, local->type);
            if (isBoxed(local) && !loops_.empty() && model_.implicitDeclarations.count(assignment->id) != 0) {
                allocateBox(local);
            }
            storeLocal(local, value);
        }
        else {
            const Type* type = model_.typeOf(assignment->target);
            llvm::Value* value = emitOwnedAs(assignment->value, type);
            const sema::FieldInfo* field = assignedField(assignment->target);
            storeHeld(emitAddress(assignment->target), value, type, field != nullptr ? field->ownership : sema::Ownership::strong);
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
        if (!blockTerminated()) {
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
        if (!blockTerminated()) {
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
        // A @[compiletime] block already ran during compilation.
        if (model_.compileTimeNodes.count(statement->id) == 0) {
            emitGroup(statement);
        }
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
    if (!blockTerminated()) {
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
        if (!blockTerminated()) {
            builder_.CreateBr(merge);
        }
    }
    builder_.SetInsertPoint(merge);
}

void Generator::emitFor(const ForStmt* statement)
{
    const sema::LocalInfo* local = model_.localOf(statement);
    const auto& statements = static_cast<const StatementGroup*>(statement->body)->statements;
    if (const sema::SendInfo* each = model_.sendOf(statement); each != nullptr && each->kind == SendKind::collectionRange) {
        emitCollectionLoop(statement->iterable, each->operandType, {local}, statements, "for");
        return;
    }
    if (const sema::SendInfo* range = model_.sendOf(statement->iterable); range != nullptr && range->kind == SendKind::enumRange) {
        const Type* element = sema::canonical(range->operandType);
        if (auto interval = nodeCast<KeywordSend>(statement->iterable)) {
            const sema::EnumInfo* info = sema::decay(element)->enumInfo;
            llvm::Value* low = enumOrdinal(emitAs(interval->receiver, element), info);
            llvm::Value* high = enumOrdinal(emitAs(interval->arguments[0], element), info);
            emitOrdinalLoop(local, element, low, high, statements, nullptr);
        }
        else {
            bool isEnumType = sema::isEnum(element);
            int64_t low = isEnumType ? 0 : element->low;
            int64_t high = isEnumType ? static_cast<int64_t>(element->enumInfo->members.size()) - 1 : element->high;
            emitOrdinalLoop(local, element, builder_.getInt64(static_cast<uint64_t>(low)), builder_.getInt64(static_cast<uint64_t>(high)), statements, nullptr);
        }
        return;
    }
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
    if (!blockTerminated()) {
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
    llvm::Value* subject = emitAs(statement->subject, type);
    bool isSigned = type->isSigned;
    llvm::Type* llvmSubject = subject->getType();
    llvm::BasicBlock* merge = llvm::BasicBlock::Create(context_, "case.end", function_);

    for (const Node* node : statement->entries) {
        auto entry = nodeCast<CaseEntry>(node);
        auto ranges = model_.caseRanges.find(entry->id);
        llvm::Value* matches = builder_.getFalse();
        for (const sema::CaseRange& range : ranges->second) {
            llvm::Value* low = llvm::ConstantInt::get(llvmSubject, range.low, isSigned);
            llvm::Value* high = llvm::ConstantInt::get(llvmSubject, range.high, isSigned);
            llvm::Value* test = nullptr;
            if (range.low == range.high) {
                test = builder_.CreateICmpEQ(subject, low);
            }
            else if (isSigned) {
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
        auto body = static_cast<const BlockLiteral*>(entry->body);
        size_t mark = temporaries_.size();
        emitStatements(body->statements);
        flushTemporaries(mark);
        if (!blockTerminated()) {
            builder_.CreateBr(merge);
        }
        builder_.SetInsertPoint(next);
    }
    if (statement->elseBody != nullptr) {
        emitGroup(statement->elseBody);
    }
    else if (model_.exhaustiveCases.count(statement->id) != 0) {
        // Every value is covered, so control can't get here.
        builder_.CreateUnreachable();
    }
    if (!blockTerminated()) {
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
    if (value != nullptr && sema::needsCleanup(model_.typeOf(expression)) && producesOwned(expression)) {
        addTemporary(value, model_.typeOf(expression));
    }
    return widen(value, model_.typeOf(expression), target);
}

llvm::Value* Generator::widen(llvm::Value* value, const Type* from, const Type* to)
{
    from = sema::canonical(from);
    to = sema::canonical(to);
    if (from == to || to == nullptr || from == nullptr || value == nullptr) {
        return value;
    }
    if (sema::isOptional(to) || sema::isSubrange(to) || sema::isSubrange(from) || from->distinctOf != nullptr) {
        return coerce(value, from, to);
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
    if (auto character = nodeCast<CharLiteral>(node)) {
        return builder_.getInt32(character->codepoint);
    }
    if (auto string = nodeCast<StringLiteral>(node)) {
        return stringLiteral(string->value);
    }
    return builder_.getInt1(static_cast<const BoolLiteral*>(node)->value);
}

llvm::Value* Generator::emit(const Node* expression)
{
    switch (expression->kind) {
    case NodeKind::integerLiteral:
    case NodeKind::realLiteral:
    case NodeKind::boolLiteral:
    case NodeKind::charLiteral:
    case NodeKind::stringLiteral:
        return emitLiteral(expression);
    case NodeKind::arrayLiteral: {
        const sema::SendInfo* send = model_.sendOf(expression);
        return send != nullptr ? emitSend(expression, *send) : nullptr;
    }
    case NodeKind::identifier:
    case NodeKind::selfExpr: {
        const Type* type = model_.typeOf(expression);
        if (auto constant = model_.constantRefs.find(expression->id); constant != model_.constantRefs.end()) {
            return materialize(constant->second->item->value, constant->second->type);
        }
        llvm::Value* value = nullptr;
        if (auto field = model_.selfFieldRefs.find(expression->id); field != model_.selfFieldRefs.end()) {
            if (field->second->ownership != sema::Ownership::strong) {
                return loadHeld(emitAddress(expression), type, field->second->ownership, field->second->name);
            }
            value = builder_.CreateLoad(llvmType(type), emitAddress(expression));
        }
        else {
            const sema::LocalInfo* local = model_.localOf(expression);
            if (local == nullptr || !sema::isValueType(local->type)) {
                return nullptr;
            }
            if (local->ownership != sema::Ownership::strong) {
                return loadHeld(addressOf(local), local->type, local->ownership, local->name);
            }
            value = builder_.CreateLoad(llvmType(local->type), addressOf(local), local->name);
        }
        // Loads that something else could overwrite mid-statement (fields, captured
        // variables) are retained so the value stays alive while it's used.
        if (producesOwned(expression)) {
            retainValue(value, type);
        }
        return value;
    }
    case NodeKind::nilLiteral:
        return llvm::ConstantPointerNull::get(builder_.getPtrTy());
    case NodeKind::blockLiteral:
        return emitBlockLiteral(static_cast<const BlockLiteral*>(expression));
    case NodeKind::fieldAccess: {
        auto access = static_cast<const FieldAccess*>(expression);
        if (auto constant = model_.constantRefs.find(access->id); constant != model_.constantRefs.end()) {
            return materialize(constant->second->item->value, constant->second->type);
        }
        const Type* type = model_.typeOf(access);
        if (llvm::Value* address = emitAddress(access)) {
            const sema::FieldInfo* field = model_.fieldOf(access);
            if (field != nullptr && field->ownership != sema::Ownership::strong) {
                return loadHeld(address, type, field->ownership, access->field);
            }
            llvm::Value* value = builder_.CreateLoad(llvmType(type), address, access->field);
            if (sema::needsCleanup(type)) {
                retainValue(value, type);
            }
            return value;
        }
        return builder_.CreateExtractValue(emit(access->receiver), {model_.fieldOf(access)->index}, access->field);
    }
    case NodeKind::attributedExpr:
        if (auto item = model_.compileTimeNodes.find(expression->id); item != model_.compileTimeNodes.end()) {
            return materialize(item->second->value, item->second->type);
        }
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
        return send.op == "and" ? builder_.CreateAnd(a, b) : send.op == "or" ? builder_.CreateOr(a, b) : builder_.CreateXor(a, b);
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
    case SendKind::constructObject:
        return emitConstruct(static_cast<const KeywordSend*>(node), send);
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
        std::vector<llvm::Value*> args{self};
        for (size_t i = 0; i < arguments.size(); ++i) {
            args.push_back(emitAs(arguments[i], send.target->params[i]->type));
        }
        llvm::FunctionType* signature = methodType(send.target);
        if (send.kind == SendKind::directMethodCall || send.kind == SendKind::superCall) {
            return builder_.CreateCall(functions_.at(send.target), args);
        }
        llvm::LoadInst* isa = builder_.CreateLoad(builder_.getPtrTy(), self, "isa");
        if (send.kind == SendKind::dynamicSend) {
            // Itable: the receiver class's witness table for the protocol, then the slot.
            // An object's class never changes and witness tables are constant, so
            // the loads are invariant and a loop can look the slot up once.
            llvm::MDNode* invariant = llvm::MDNode::get(context_, {});
            isa->setMetadata(llvm::LLVMContext::MD_invariant_load, invariant);
            llvm::Value* witness = builder_.CreateCall(witnessFunction(), {isa, protocolGlobal(send.classInfo)}, "witness");
            llvm::Value* entry = builder_.CreateConstInBoundsGEP1_64(builder_.getPtrTy(), witness, static_cast<uint64_t>(send.slot));
            llvm::LoadInst* imp = builder_.CreateLoad(builder_.getPtrTy(), entry, "imp");
            imp->setMetadata(llvm::LLVMContext::MD_invariant_load, invariant);
            return builder_.CreateCall(signature, imp, args);
        }
        uint64_t offset = abi::classVtableOffset + static_cast<uint64_t>(send.slot) * 8;
        llvm::Value* entry = builder_.CreateConstInBoundsGEP1_64(builder_.getInt8Ty(), isa, offset);
        llvm::Value* imp = builder_.CreateLoad(builder_.getPtrTy(), entry, "imp");
        return builder_.CreateCall(signature, imp, args);
    }
    case SendKind::fieldGet: {
        auto unary = static_cast<const UnarySend*>(node);
        llvm::Value* address = fieldAddress(emitAs(unary->receiver, nullptr), send.field);
        if (send.field->ownership != sema::Ownership::strong) {
            return loadHeld(address, send.field->type, send.field->ownership, send.field->name);
        }
        llvm::Value* value = builder_.CreateLoad(llvmType(send.field->type), address);
        if (sema::needsCleanup(send.field->type)) {
            retainValue(value, send.field->type);
        }
        return value;
    }
    case SendKind::fieldSet: {
        auto keyword = static_cast<const KeywordSend*>(node);
        llvm::Value* object = emitAs(keyword->receiver, nullptr);
        llvm::Value* value = emitOwnedAs(keyword->arguments[0], send.field->type);
        storeHeld(fieldAddress(object, send.field), value, send.field->type, send.field->ownership);
        return nullptr;
    }
    case SendKind::referenceCompare: {
        auto binary = static_cast<const BinarySend*>(node);
        llvm::Value* a = emitAs(binary->receiver, nullptr);
        llvm::Value* b = emitAs(binary->argument, nullptr);
        return send.op == "=" ? builder_.CreateICmpEQ(a, b) : builder_.CreateICmpNE(a, b);
    }
    case SendKind::constantValue:
    case SendKind::metaInfo:
    case SendKind::metaFields:
    case SendKind::compilerDiagnostic:
        return emitCompileTimeSend(node, send);
    case SendKind::optionalCompare:
        return emitOptionalCompare(static_cast<const BinarySend*>(node), send);
    case SendKind::inlineIfNil:
        return emitInlinedIfNil(static_cast<const KeywordSend*>(node), send);
    default:
        if (llvm::Value* value = emitEnumSend(node, send)) {
            return value;
        }
        if (llvm::Value* value = emitBitSetSend(node, send)) {
            return value;
        }
        if (llvm::Value* value = emitStringSend(node, send)) {
            return value;
        }
        return emitContainerSend(node, send);
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// blocks

llvm::Function* Generator::emitBlockFunction(const sema::BlockInfo* info)
{
    llvm::Function* savedFunction = function_;
    const sema::BlockInfo* savedBlock = block_;
    llvm::Value* savedEnvironment = environment_;
    auto savedSlots = std::move(slots_);
    auto savedLoops = std::move(loops_);
    auto savedTemporaries = std::move(temporaries_);
    auto savedStack = std::move(stackSlots_);
    stackSlots_.clear();
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

    createSlots(info->locals);
    for (size_t i = 0; i < info->params.size(); ++i) {
        builder_.CreateStore(function_->getArg(static_cast<unsigned>(i + 1)), slots_.at(info->params[i]));
    }
    allocateStackObjects(message_, info);

    llvm::Value* result = emitInlinedBody(info->node, blockType->result);
    if (!blockTerminated()) {
        emitReturn(blockType->result->kind == TypeKind::voidType ? nullptr : result);
    }

    llvm::Function* function = function_;
    function_ = savedFunction;
    block_ = savedBlock;
    environment_ = savedEnvironment;
    slots_ = std::move(savedSlots);
    loops_ = std::move(savedLoops);
    temporaries_ = std::move(savedTemporaries);
    stackSlots_ = std::move(savedStack);
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
    if (sema::needsCleanup(resultType)) {
        return emitOwnedAs(last->expression, resultType);
    }
    return emitAs(last->expression, resultType);
}

llvm::Value* Generator::emitInvoke(const Node* receiver, const std::vector<Node*>& arguments, const Type* blockType)
{
    llvm::Value* block = emitAs(receiver, nullptr);
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
        if (!blockTerminated()) {
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
    if (!blockTerminated()) {
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

namespace bob::codegen {

std::unique_ptr<llvm::Module> generateModule(const ast::Unit* unit, const sema::SemanticModel& model, llvm::LLVMContext& context,
                                             const std::string& dataLayout)
{
    detail::Generator generator(model, context, dataLayout);
    return generator.run(unit);
}

}
