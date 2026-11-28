// Pointers and slices (design §6.5): `pointer to: T`, `volatilePointer to: T`
// and `slice of: T`. Making a pointer and reaching memory through one needs an
// unsafe region; holding, passing and comparing pointers, and using slices,
// doesn't.

#include "analyzerInternal.h"

namespace bob::sema::detail {

namespace {

const Type* pointerContext(const Type* expected)
{
    const Type* target = expected != nullptr ? canonical(expected) : nullptr;
    if (target == nullptr) {
        return nullptr;
    }
    if (isPointer(target) || (isOptional(target) && isPointer(target->inner))) {
        return target;
    }
    return nullptr;
}

const Type* pointerOf(const Type* type)
{
    type = canonical(type);
    return isOptional(type) ? canonical(type->inner) : type;
}

}

bool Analyzer::reachesThroughPointer(const Node* node) const
{
    auto access = nodeCast<FieldAccess>(node);
    if (access == nullptr) {
        return false;
    }
    return isPointer(model_.typeOf(access->receiver)) || reachesThroughPointer(access->receiver);
}

// `x address`, `r.field address`, `p.field address`: a pointer to a place.
const Type* Analyzer::checkAddress(const UnarySend* send, const Type* receiver)
{
    if (!requireUnsafe(send, "'address'")) {
        return types_.error();
    }
    const Node* place = send->receiver;
    while (auto attributed = nodeCast<AttributedExpr>(place)) {
        place = attributed->expression;
    }
    bool isVolatile = false;
    bool frameBound = true;
    for (const Node* node = place; auto access = nodeCast<FieldAccess>(node); node = access->receiver) {
        const Type* base = canonical(model_.typeOf(access->receiver));
        if (isPointer(base)) {
            isVolatile = base->isVolatile;
            frameBound = false;
            if (base->inner->kind == TypeKind::record && base->inner->record->packed) {
                error(send, "sema.packedAddress", "a field of the @[packed] record '" + typeName(base->inner) + "' may be unaligned, so it has no address");
                return types_.error();
            }
            break;
        }
        if (isClass(base)) {
            error(send, "sema.invalidAddress",
                  "a field of an object has no address: reference counting may free the object while the pointer lives");
            return types_.error();
        }
        if (base->kind == TypeKind::record && base->record->packed) {
            error(send, "sema.packedAddress", "a field of the @[packed] record '" + typeName(base) + "' may be unaligned, so it has no address");
            return types_.error();
        }
    }
    if (frameBound) {
        const Node* root = place;
        while (auto access = nodeCast<FieldAccess>(root)) {
            root = access->receiver;
        }
        const LocalInfo* local = nodeCast<Identifier>(root) != nullptr && model_.selfFieldRefs.count(root->id) == 0 ? model_.localOf(root) : nullptr;
        if (local == nullptr) {
            error(send, "sema.invalidAddress",
                  model_.selfFieldRefs.count(root->id) != 0
                      ? std::string("a field of an object has no address: reference counting may free the object while the pointer lives")
                      : std::string("only a variable, a field of a record variable, or a place reached through a pointer has an address"));
            return types_.error();
        }
        if (local->readOnly) {
            error(send, "sema.invalidAddress", "'" + local->name + "' is read-only, so it has no address; copy it into a variable first");
            return types_.error();
        }
    }
    if (!isPointee(receiver)) {
        error(send, "sema.invalidPointee",
              "a pointer points at plain data, so a '" + typeName(receiver) + "' (reference counted or without a fixed layout) has no address");
        return types_.error();
    }
    const Type* result = types_.pointerTo(canonical(receiver), isVolatile);
    addSend(send, SendKind::pointerAddress, "address", result, result);
    model_.sends[send->id].frameBound = frameBound;
    return result;
}

const Type* Analyzer::checkPointerUnary(const UnarySend* send, const Type* receiver, const Type* expected)
{
    const std::string& selector = send->selector;
    const Type* type = canonical(receiver);
    if (selector == "address" && !isClass(type) && !isOptional(type)) {
        return checkAddress(send, receiver);
    }
    if (selector == "asPointer" && (isInteger(decay(type)) || isPointer(type))) {
        const Type* target = pointerContext(expected);
        if (target == nullptr) {
            error(send->selectorRange, "sema.pointerNeedsType",
                  "'asPointer' takes its pointer type from context; declare it, as in 'p: pointer to: uint32 := n asPointer', or write 'asPointer: T'");
            return types_.error();
        }
        if (!requireUnsafe(send, "'asPointer'")) {
            return types_.error();
        }
        if (isPointer(type)) {
            if (type->isVolatile && !pointerOf(target)->isVolatile) {
                error(send->selectorRange, "sema.typeMismatch",
                      "a volatilePointer can't become a plain pointer: accesses through it would no longer be volatile");
                return types_.error();
            }
            addSend(send, SendKind::pointerCast, selector, type, target);
            return target;
        }
        if (decay(type)->bits != 64 || decay(type)->isSigned) {
            error(send, "sema.typeMismatch", "an address is a 'uintptr' (uint64), not a '" + typeName(type) + "'");
            return types_.error();
        }
        addSend(send, SendKind::pointerFromInteger, selector, decay(type), target);
        return target;
    }
    if (isPointer(type)) {
        if (selector == "asUIntPtr") {
            addSend(send, SendKind::pointerToInteger, selector, type, types_.integer(64, false));
            return types_.integer(64, false);
        }
        if (selector == "contents") {
            if (!requireUnsafe(send, "'contents'")) {
                return types_.error();
            }
            addSend(send, SendKind::pointerLoad, selector, type, type->inner);
            return type->inner;
        }
        error(send->selectorRange, "sema.unknownMessage",
              "'" + typeName(type) + "' doesn't understand '" + selector +
                  "'; a pointer understands contents, contents:, advancedBy:, slice:, asPointer, asPointer: and asUIntPtr");
        return types_.error();
    }
    if (isSlice(type)) {
        if (selector == "count" || selector == "isEmpty") {
            bool count = selector == "count";
            const Type* result = count ? types_.int64() : types_.boolType();
            addSend(send, count ? SendKind::sliceCount : SendKind::sliceIsEmpty, selector, type, result);
            return result;
        }
        error(send->selectorRange, "sema.unknownMessage",
              "'" + typeName(type) + "' doesn't understand '" + selector + "'; a slice understands count, isEmpty, at:, at:put: and for … in");
        return types_.error();
    }
    return nullptr;
}

const Type* Analyzer::checkPointerKeyword(const KeywordSend* send, const Type* receiver)
{
    const std::string& selector = send->selector;
    const Type* type = canonical(receiver);
    SourceRange selectorRange = send->partRanges.empty() ? send->range : send->partRanges.front();
    if (isSlice(type)) {
        if (selector == "at:" || selector == "at:put:") {
            checkArgument(send->arguments[0], types_.int64());
            bool put = selector == "at:put:";
            if (put) {
                checkArgument(send->arguments[1], type->inner);
            }
            const Type* result = put ? types_.voidType() : type->inner;
            addSend(send, put ? SendKind::sliceAtPut : SendKind::sliceAt, selector, type, result);
            return result;
        }
        for (const Node* argument : send->arguments) {
            check(argument, nullptr);
        }
        error(selectorRange, "sema.unknownMessage",
              "'" + typeName(type) + "' doesn't understand '" + selector + "'; a slice understands count, isEmpty, at:, at:put: and for … in");
        return types_.error();
    }
    if (!isPointer(type)) {
        return nullptr;
    }
    if (selector == "contents:") {
        checkArgument(send->arguments[0], type->inner);
        if (!requireUnsafe(send, "'contents:'")) {
            return types_.error();
        }
        addSend(send, SendKind::pointerStore, selector, type, types_.voidType());
        return types_.voidType();
    }
    if (selector == "advancedBy:" || selector == "slice:") {
        checkArgument(send->arguments[0], types_.int64());
        bool slice = selector == "slice:";
        if (!requireUnsafe(send, "'" + selector + "'")) {
            return types_.error();
        }
        if (slice && type->isVolatile) {
            error(selectorRange, "sema.typeMismatch", "a slice reads memory with ordinary loads, so it can't be made from a volatilePointer");
            return types_.error();
        }
        const Type* result = slice ? types_.sliceOf(type->inner) : type;
        addSend(send, slice ? SendKind::pointerSlice : SendKind::pointerAdvance, selector, type, result);
        return result;
    }
    if (selector == "asPointer:") {
        const Type* meta = check(send->arguments[0], nullptr);
        if (meta == types_.error()) {
            return types_.error();
        }
        if (meta->kind != TypeKind::meta || meta->instance == nullptr) {
            error(send->arguments[0], "sema.expectedType", "'asPointer:' takes the type to point at, such as 'p asPointer: uint8'");
            return types_.error();
        }
        const Type* pointee = canonical(meta->instance);
        if (!isPointee(pointee)) {
            error(send->arguments[0], "sema.invalidPointee", "a pointer points at plain data, not a '" + typeName(pointee) + "'");
            return types_.error();
        }
        if (!requireUnsafe(send, "'asPointer:'")) {
            return types_.error();
        }
        const Type* result = types_.pointerTo(pointee, type->isVolatile);
        addSend(send, SendKind::pointerCast, selector, type, result);
        return result;
    }
    for (const Node* argument : send->arguments) {
        check(argument, nullptr);
    }
    error(selectorRange, "sema.unknownMessage",
          "'" + typeName(type) + "' doesn't understand '" + selector +
              "'; a pointer understands contents, contents:, advancedBy:, slice:, asPointer, asPointer: and asUIntPtr");
    return types_.error();
}

}
