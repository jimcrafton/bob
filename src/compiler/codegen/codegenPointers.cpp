// Pointers and slices (design §6.5). A pointer is a plain `ptr`; a slice is
// { ptr, i64 count }. Every access through a volatilePointer is one volatile
// load or store of exactly the accessed type.

#include "codegenInternal.h"
#include "layout.h"

namespace bob::codegen::detail {

namespace {

const Type* pointerOf(const Type* type)
{
    type = sema::canonical(type);
    return sema::isOptional(type) ? sema::canonical(type->inner) : type;
}

}

bool Generator::placeAccess(const Node* place, llvm::Align& align) const
{
    bool isVolatile = false;
    bool packed = false;
    for (const Node* node = place; auto access = nodeCast<FieldAccess>(node); node = access->receiver) {
        const Type* base = sema::canonical(model_.typeOf(access->receiver));
        if (sema::isPointer(base)) {
            isVolatile = isVolatile || base->isVolatile;
            base = base->inner;
        }
        packed = packed || (base->kind == TypeKind::record && base->record->packed);
    }
    align = llvm::Align(packed ? 1 : sema::alignmentOf(model_.typeOf(place)));
    return isVolatile;
}

bool Generator::throughPointer(const Node* place) const
{
    auto access = nodeCast<FieldAccess>(place);
    return access != nullptr && (sema::isPointer(model_.typeOf(access->receiver)) || throughPointer(access->receiver));
}

llvm::Value* Generator::emitPointerSend(const Node* node, const sema::SendInfo& send)
{
    llvm::Type* ptr = builder_.getPtrTy();
    llvm::Type* i64 = builder_.getInt64Ty();
    const Node* receiver = nullptr;
    std::vector<const Node*> arguments;
    if (auto unary = nodeCast<UnarySend>(node)) {
        receiver = unary->receiver;
    }
    else if (auto keyword = nodeCast<KeywordSend>(node)) {
        receiver = keyword->receiver;
        arguments.assign(keyword->arguments.begin(), keyword->arguments.end());
    }
    const Type* type = sema::canonical(send.operandType);
    auto pointeeAlign = [&](const Type* pointer) { return llvm::Align(sema::alignmentOf(pointerOf(pointer)->inner)); };
    switch (send.kind) {
    case SendKind::pointerAddress: {
        const Node* place = receiver;
        while (auto attributed = nodeCast<AttributedExpr>(place)) {
            place = attributed->expression;
        }
        return emitAddress(place);
    }
    case SendKind::pointerLoad:
        return builder_.CreateAlignedLoad(llvmType(type->inner), emitAs(receiver, nullptr), pointeeAlign(type), type->isVolatile, "contents");
    case SendKind::pointerStore: {
        llvm::Value* pointer = emitAs(receiver, nullptr);
        builder_.CreateAlignedStore(emitAs(arguments[0], type->inner), pointer, pointeeAlign(type), type->isVolatile);
        return nullptr;
    }
    case SendKind::pointerAdvance:
        return builder_.CreateGEP(llvmType(type->inner), emitAs(receiver, nullptr), {emitAs(arguments[0], model_.types.int64())}, "advanced");
    case SendKind::pointerFromInteger: {
        // Checked: 0 only into an optional pointer, and aligned for what it points at.
        llvm::Value* address = emitAs(receiver, type);
        uint64_t align = sema::alignmentOf(pointerOf(send.resultType)->inner);
        if (align > 1) {
            trapIf(builder_.CreateICmpNE(builder_.CreateAnd(address, builder_.getInt64(align - 1)), builder_.getInt64(0)), abi::TrapReason::invalidPointer);
        }
        if (sema::isPointer(send.resultType)) {
            trapIf(builder_.CreateICmpEQ(address, builder_.getInt64(0)), abi::TrapReason::invalidPointer);
        }
        return builder_.CreateIntToPtr(address, ptr, "pointer");
    }
    case SendKind::pointerCast: {
        llvm::Value* pointer = emitAs(receiver, nullptr);
        uint64_t from = sema::alignmentOf(type->inner);
        uint64_t to = sema::alignmentOf(pointerOf(send.resultType)->inner);
        if (to > from) {
            llvm::Value* address = builder_.CreatePtrToInt(pointer, i64);
            trapIf(builder_.CreateICmpNE(builder_.CreateAnd(address, builder_.getInt64(to - 1)), builder_.getInt64(0)), abi::TrapReason::invalidPointer);
        }
        return pointer;
    }
    case SendKind::pointerToInteger:
        return builder_.CreatePtrToInt(emitAs(receiver, nullptr), i64, "address");
    case SendKind::pointerSlice: {
        llvm::Value* pointer = emitAs(receiver, nullptr);
        llvm::Value* count = emitAs(arguments[0], model_.types.int64());
        trapIf(builder_.CreateICmpSLT(count, builder_.getInt64(0)), abi::TrapReason::valueOutOfRange);
        llvm::Value* slice = llvm::UndefValue::get(llvmType(send.resultType));
        slice = builder_.CreateInsertValue(slice, pointer, {0});
        return builder_.CreateInsertValue(slice, count, {1});
    }
    case SendKind::sliceCount:
    case SendKind::sliceIsEmpty: {
        llvm::Value* count = builder_.CreateExtractValue(emitAs(receiver, nullptr), {1}, "count");
        return send.kind == SendKind::sliceCount ? count : builder_.CreateICmpEQ(count, builder_.getInt64(0));
    }
    case SendKind::sliceAt:
    case SendKind::sliceAtPut: {
        llvm::Value* slice = emitAs(receiver, nullptr);
        llvm::Value* index = emitAs(arguments[0], model_.types.int64());
        trapIf(builder_.CreateICmpUGE(index, builder_.CreateExtractValue(slice, {1})), abi::TrapReason::indexOutOfRange);
        llvm::Value* address = builder_.CreateInBoundsGEP(llvmType(type->inner), builder_.CreateExtractValue(slice, {0}), {index});
        if (send.kind == SendKind::sliceAt) {
            return builder_.CreateLoad(llvmType(type->inner), address, "element");
        }
        builder_.CreateStore(emitAs(arguments[1], type->inner), address);
        return nullptr;
    }
    default:
        return nullptr;
    }
}

}
