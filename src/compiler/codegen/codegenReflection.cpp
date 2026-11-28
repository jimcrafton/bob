// Reflection through the class object (design §9). A `Class` is a pointer to
// the class's descriptor (BobClass in objectModel.h).

#include "codegenInternal.h"
#include "layout.h"
#include "objectModel.h"

#include <llvm/Transforms/Utils/ModuleUtils.h>

namespace bob::codegen::detail {

namespace {

// Identifies a type for `get:of:`'s runtime check; class names are qualified by
// their unit so two modules' `Button`s don't match.
std::string typeKey(const Type* type)
{
    type = sema::canonical(type);
    if (type->kind == TypeKind::classType && type->classInfo != nullptr) {
        return type->classInfo->unitName + "." + type->classInfo->name;
    }
    return sema::typeName(type);
}

// "int64,string>bool": what `perform:` checks a call against (BobMessageEntry).
std::string signatureKey(const std::vector<const Type*>& arguments, const Type* result)
{
    std::string key;
    for (size_t i = 0; i < arguments.size(); ++i) {
        key += (i == 0 ? "" : ",") + typeKey(arguments[i]);
    }
    bool returnsValue = result != nullptr && sema::canonical(result)->kind != TypeKind::voidType;
    return key + ">" + (returnsValue ? typeKey(result) : std::string("void"));
}

}

llvm::Function* Generator::fieldOffsetFunction()
{
    llvm::Type* ptr = builder_.getPtrTy();
    return runtimeFunction("bobClassFieldOffset", builder_.getInt64Ty(), {ptr, ptr, ptr, ptr, builder_.getInt32Ty()});
}

// BobClassMeta (objectModel.h): the class's own public fields, its own sendable
// instance messages with their signatures, and every protocol it conforms to.
llvm::Constant* Generator::emitClassMeta(const sema::ClassInfo* info)
{
    llvm::Type* ptr = builder_.getPtrTy();
    llvm::Type* i64 = builder_.getInt64Ty();
    llvm::Type* i32 = builder_.getInt32Ty();
    if (info->reflectLevel < sema::ReflectLevel::introspect) {
        return llvm::ConstantPointerNull::get(builder_.getPtrTy());
    }
    std::string suffix = info->unitName + "." + info->name;
    auto constantArray = [&](llvm::Type* element, const std::vector<llvm::Constant*>& values, const std::string& name) -> llvm::Constant* {
        if (values.empty()) {
            return llvm::ConstantPointerNull::get(builder_.getPtrTy());
        }
        llvm::ArrayType* type = llvm::ArrayType::get(element, values.size());
        return new llvm::GlobalVariable(*module_, type, true, llvm::GlobalValue::PrivateLinkage, llvm::ConstantArray::get(type, values), name);
    };

    llvm::StructType* fieldType = llvm::StructType::get(context_, {ptr, ptr, ptr, i64, i64, i32, i32});
    std::vector<llvm::Constant*> fields;
    for (const sema::FieldInfo& field : info->fields) {
        if (field.owner != info || !field.isPublic) {
            continue;
        }
        fields.push_back(llvm::ConstantStruct::get(
            fieldType, {stringLiteral(field.name), stringLiteral(sema::typeName(field.type)), stringLiteral(typeKey(field.type)),
                        builder_.getInt64(sema::objectFieldOffset(info, field.index)), builder_.getInt64(sema::sizeOf(field.type)),
                        builder_.getInt32(static_cast<uint32_t>(field.ownership)), builder_.getInt32(0)}));
    }

    bool invocable = info->reflectLevel >= sema::ReflectLevel::invoke;
    llvm::StructType* messageType = llvm::StructType::get(context_, {ptr, ptr, ptr, i64, ptr, ptr});
    std::vector<llvm::Constant*> messages;
    for (const sema::MessageInfo* method : info->methodList) {
        if (method->isInit || method->isCopy || method->compileTimeOnly || functions_.count(method) == 0) {
            continue;
        }
        std::vector<llvm::Constant*> arguments;
        std::vector<const Type*> argumentTypes;
        for (const sema::LocalInfo* param : method->params) {
            arguments.push_back(stringLiteral(sema::typeName(param->type)));
            argumentTypes.push_back(param->type);
        }
        bool returnsValue = method->returnType != nullptr && sema::canonical(method->returnType)->kind != TypeKind::voidType;
        llvm::Constant* imp = invocable ? static_cast<llvm::Constant*>(reflectionThunk(method)) : llvm::ConstantPointerNull::get(builder_.getPtrTy());
        messages.push_back(llvm::ConstantStruct::get(
            messageType, {stringLiteral(method->selector), stringLiteral(returnsValue ? sema::typeName(method->returnType) : "void"),
                          constantArray(ptr, arguments, "meta.args." + suffix + "." + method->selector), builder_.getInt64(arguments.size()),
                          stringLiteral(signatureKey(argumentTypes, method->returnType)), imp}));
    }

    std::vector<llvm::Constant*> protocols;
    for (const sema::ClassInfo* protocol : info->conformedProtocols()) {
        protocols.push_back(stringLiteral(protocol->name));
    }

    llvm::Constant* create = invocable && info->defaultable ? static_cast<llvm::Constant*>(newFunctions_.at(info))
                                                            : llvm::ConstantPointerNull::get(builder_.getPtrTy());
    llvm::StructType* metaType = llvm::StructType::get(context_, {ptr, i64, ptr, i64, ptr, i64, ptr, ptr, i32, i32});
    llvm::Constant* meta = llvm::ConstantStruct::get(
        metaType, {constantArray(fieldType, fields, "meta.fields." + suffix), builder_.getInt64(fields.size()),
                   constantArray(messageType, messages, "meta.messages." + suffix), builder_.getInt64(messages.size()),
                   constantArray(ptr, protocols, "meta.protocols." + suffix), builder_.getInt64(protocols.size()),
                   stringLiteral(suffix), create, builder_.getInt32(static_cast<uint32_t>(info->reflectLevel)), builder_.getInt32(0)});
    // Classes at `invoke` join the program-wide registry `Class named:` searches.
    // Compile-time code can't use it, so the evaluator's modules leave it out.
    if (invocable && evaluating_ == nullptr) {
        auto entry = new llvm::GlobalVariable(*module_, ptr, true, llvm::GlobalValue::PrivateLinkage, classGlobals_.at(info), "registry." + suffix);
        entry->setSection(abi::classRegistrySection);
        entry->setAlignment(llvm::Align(8));
        llvm::appendToUsed(*module_, {entry});
    }
    return new llvm::GlobalVariable(*module_, metaType, true, llvm::GlobalValue::PrivateLinkage, meta, "meta." + suffix);
}

llvm::Value* Generator::emitReflectionSend(const Node* node, const sema::SendInfo& send)
{
    llvm::Type* ptr = builder_.getPtrTy();
    llvm::Type* i8 = builder_.getInt8Ty();
    const Node* receiver = nullptr;
    const Node* argument = nullptr;
    if (auto unary = nodeCast<UnarySend>(node)) {
        receiver = unary->receiver;
    }
    else if (auto binary = nodeCast<BinarySend>(node)) {
        receiver = binary->receiver;
        argument = binary->argument;
    }
    else if (auto keyword = nodeCast<KeywordSend>(node)) {
        receiver = keyword->receiver;
        argument = keyword->arguments.empty() ? nullptr : keyword->arguments[0];
    }
    switch (send.kind) {
    case SendKind::classOf: {
        // An object's class never changes.
        llvm::LoadInst* isa = builder_.CreateLoad(ptr, emitAs(receiver, nullptr), "class");
        isa->setMetadata(llvm::LLVMContext::MD_invariant_load, llvm::MDNode::get(context_, {}));
        return isa;
    }
    case SendKind::namedClass:
        return classGlobals_.at(send.classInfo);
    case SendKind::className:
        // An immortal string constant, or null (the empty string) below `identity`.
        return builder_.CreateLoad(ptr, builder_.CreateConstInBoundsGEP1_64(i8, emitAs(receiver, nullptr), 8), "class.name");
    case SendKind::classSuperclass: {
        llvm::Value* superclass = builder_.CreateLoad(ptr, emitAs(receiver, nullptr), "superclass");
        llvm::Value* value = llvm::UndefValue::get(llvmType(send.resultType));
        value = builder_.CreateInsertValue(value, builder_.CreateIsNotNull(superclass), {0});
        return builder_.CreateInsertValue(value, superclass, {1});
    }
    case SendKind::classIsKindOf: {
        llvm::Value* result = builder_.CreateCall(runtimeFunction("bobClassIsKindOf", i8, {ptr, ptr}), {emitAs(receiver, nullptr), emitAs(argument, nullptr)});
        return builder_.CreateICmpNE(result, builder_.getInt8(0));
    }
    case SendKind::classRespondsTo: {
        llvm::Value* result = builder_.CreateCall(runtimeFunction("bobClassRespondsTo", i8, {ptr, ptr}),
                                                  {emitAs(receiver, nullptr), emitAs(argument, model_.types.string())});
        return builder_.CreateICmpNE(result, builder_.getInt8(0));
    }
    case SendKind::classCompare: {
        llvm::Value* a = emitAs(receiver, nullptr);
        llvm::Value* b = emitAs(argument, nullptr);
        return send.op == "=" ? builder_.CreateICmpEQ(a, b) : builder_.CreateICmpNE(a, b);
    }
    case SendKind::classFields:
    case SendKind::classMessages: {
        llvm::Value* cls = emitAs(receiver, nullptr);
        llvm::Value* metaClass = classGlobals_.at(send.classInfo);
        llvm::Value* metaElement = elementDescriptor(send.classInfo->type);
        if (send.kind == SendKind::classFields) {
            return builder_.CreateCall(runtimeFunction("bobClassFields", ptr, {ptr, ptr, ptr}), {cls, metaClass, metaElement});
        }
        return builder_.CreateCall(runtimeFunction("bobClassMessages", ptr, {ptr, ptr, ptr, ptr}),
                                   {cls, metaClass, metaElement, elementDescriptor(model_.types.string())});
    }
    case SendKind::classProtocols:
        return builder_.CreateCall(runtimeFunction("bobClassProtocols", ptr, {ptr, ptr}),
                                   {emitAs(receiver, nullptr), elementDescriptor(model_.types.string())});
    case SendKind::classGetField: {
        auto keyword = static_cast<const KeywordSend*>(node);
        llvm::Value* cls = emitAs(receiver, nullptr);
        llvm::Value* name = emitAs(keyword->arguments[0], model_.types.string());
        llvm::Value* object = emitAs(keyword->arguments[1], nullptr);
        llvm::Value* offset = builder_.CreateCall(fieldOffsetFunction(), {cls, object, name, stringLiteral(typeKey(send.resultType)), builder_.getInt32(0)},
                                                  "field.offset");
        llvm::Value* value = builder_.CreateLoad(llvmType(send.resultType), builder_.CreateInBoundsGEP(i8, object, offset), "field");
        retainValue(value, send.resultType);
        return value;
    }
    case SendKind::classSetField: {
        auto keyword = static_cast<const KeywordSend*>(node);
        llvm::Value* cls = emitAs(receiver, nullptr);
        llvm::Value* name = emitAs(keyword->arguments[0], model_.types.string());
        llvm::Value* object = emitAs(keyword->arguments[1], nullptr);
        llvm::Value* value = emitOwnedAs(keyword->arguments[2], send.operandType);
        llvm::Value* offset = builder_.CreateCall(fieldOffsetFunction(), {cls, object, name, stringLiteral(typeKey(send.operandType)), builder_.getInt32(1)},
                                                  "field.offset");
        storeHeld(builder_.CreateInBoundsGEP(i8, object, offset), value, send.operandType, sema::Ownership::strong);
        return nullptr;
    }
    case SendKind::classPerform: {
        // Through the message's `.msgSend` thunk: (self, selector, args...).
        auto keyword = static_cast<const KeywordSend*>(node);
        llvm::Value* cls = emitAs(receiver, nullptr);
        llvm::Value* selector = emitAs(keyword->arguments[0], model_.types.string());
        llvm::Value* object = emitAs(keyword->arguments[1], nullptr);
        std::vector<llvm::Value*> args{object, llvm::ConstantPointerNull::get(builder_.getPtrTy())};
        std::vector<llvm::Type*> params{ptr, ptr};
        for (size_t i = 0; i < send.argumentTypes.size(); ++i) {
            args.push_back(emitAs(keyword->arguments[i + 2], send.argumentTypes[i]));
            params.push_back(llvmType(send.argumentTypes[i]));
        }
        llvm::Value* imp = builder_.CreateCall(runtimeFunction("bobClassMethod", ptr, {ptr, ptr, ptr, ptr}),
                                               {cls, object, selector, stringLiteral(signatureKey(send.argumentTypes, send.resultType))}, "imp");
        llvm::CallInst* call = builder_.CreateCall(llvm::FunctionType::get(llvmType(send.resultType), params, false), imp, args);
        return call->getType()->isVoidTy() ? nullptr : call;
    }
    case SendKind::classNew: {
        llvm::Value* null = llvm::ConstantPointerNull::get(builder_.getPtrTy());
        bool protocol = send.classInfo->isProtocol;
        llvm::Value* expectedClass = protocol ? null : static_cast<llvm::Value*>(classGlobals_.at(send.classInfo));
        llvm::Value* expectedProtocol = protocol ? static_cast<llvm::Value*>(protocolGlobal(send.classInfo)) : null;
        return builder_.CreateCall(runtimeFunction("bobClassNew", ptr, {ptr, ptr, ptr}), {emitAs(receiver, nullptr), expectedClass, expectedProtocol},
                                   "new");
    }
    case SendKind::classNamed: {
        llvm::Value* found = builder_.CreateCall(runtimeFunction("bobClassNamed", ptr, {ptr}), {emitAs(argument, model_.types.string())}, "class");
        llvm::Value* value = llvm::UndefValue::get(llvmType(send.resultType));
        value = builder_.CreateInsertValue(value, builder_.CreateIsNotNull(found), {0});
        return builder_.CreateInsertValue(value, found, {1});
    }
    case SendKind::classQualifiedName: {
        // BobClass.meta (at 64), then BobClassMeta.qualifiedName (at 48); "" without one.
        llvm::Value* meta = builder_.CreateLoad(ptr, builder_.CreateConstInBoundsGEP1_64(i8, emitAs(receiver, nullptr), 64), "meta");
        llvm::BasicBlock* from = builder_.GetInsertBlock();
        llvm::BasicBlock* read = llvm::BasicBlock::Create(context_, "meta.name", function_);
        llvm::BasicBlock* done = llvm::BasicBlock::Create(context_, "meta.done", function_);
        builder_.CreateCondBr(builder_.CreateIsNotNull(meta), read, done);
        builder_.SetInsertPoint(read);
        llvm::Value* name = builder_.CreateLoad(ptr, builder_.CreateConstInBoundsGEP1_64(i8, meta, 48), "qualifiedName");
        builder_.CreateBr(done);
        builder_.SetInsertPoint(done);
        llvm::PHINode* result = builder_.CreatePHI(ptr, 2, "qualifiedName");
        result->addIncoming(llvm::ConstantPointerNull::get(builder_.getPtrTy()), from);
        result->addIncoming(name, read);
        return result;
    }
    case SendKind::objectIsOnStack: {
        llvm::Value* flags = builder_.CreateLoad(builder_.getInt32Ty(), builder_.CreateConstInBoundsGEP1_64(i8, emitAs(receiver, nullptr), 12), "flags");
        return builder_.CreateICmpNE(builder_.CreateAnd(flags, builder_.getInt32(bobObjectStack)), builder_.getInt32(0));
    }
    default:
        return nullptr;
    }
}

}
