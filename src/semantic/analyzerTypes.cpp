// Enums, named types, subranges, enum sets and optionals of plain values (phase 5).

#include "analyzerInternal.h"

#include <algorithm>

namespace bob::sema::detail {

namespace {

int64_t signedValue(uint64_t bits, const Type* storage)
{
    if (!storage->isSigned || storage->bits == 64) {
        return static_cast<int64_t>(bits);
    }
    uint64_t sign = uint64_t(1) << (storage->bits - 1);
    uint64_t mask = (uint64_t(1) << storage->bits) - 1;
    bits &= mask;
    return static_cast<int64_t>((bits ^ sign) - sign);
}

uint64_t literalBits(const IntegerLiteral* literal)
{
    return literal->negative ? (~literal->value + 1) : literal->value;
}

}

// ---------------------------------------------------------------------------
// enums

void Analyzer::declareEnum(const EnumDecl* decl)
{
    if (declaredHere(decl->name)) {
        error(decl->nameRange, "sema.redeclared", "'" + decl->name + "' is already declared");
        return;
    }
    for (const Attribute* attribute : decl->attributes) {
        unsupported(attribute, "the enum attribute '@[" + attribute->name + "]'", "a later phase");
    }
    auto info = std::make_unique<EnumInfo>();
    info->name = decl->name;
    info->origin = currentOrigin();
    info->decl = decl;
    model_.enums.push_back(std::move(info));
}

void Analyzer::resolveEnum(EnumInfo* info)
{
    if (info->type != nullptr) {
        return;
    }
    const EnumDecl* decl = info->decl;
    const Type* storage = types_.int32();
    if (decl->storageType != nullptr) {
        const Type* declared = resolveType(decl->storageType);
        if (isInteger(declared) && declared->distinctOf == nullptr) {
            storage = declared;
        }
        else if (declared != types_.error()) {
            error(decl->storageType, "sema.invalidEnumStorage", "an enum's storage type must be an integer type, not '" + typeName(declared) + "'");
        }
    }
    info->storage = storage;
    info->type = types_.addEnum(info, storage);

    int64_t next = 0;
    bool nextValid = true;
    for (const Node* node : decl->members) {
        auto member = nodeCast<EnumMember>(node);
        if (member == nullptr) {
            continue;
        }
        if (info->member(member->name) != nullptr) {
            error(member->nameRange, "sema.redeclared", "'" + info->name + "' already has a member '" + member->name + "'");
            continue;
        }
        uint64_t bits = 0;
        if (member->value != nullptr) {
            auto literal = nodeCast<IntegerLiteral>(member->value);
            if (literal == nullptr || !literal->suffix.empty()) {
                error(member->value, "sema.enumValueNotConstant", "an enum member's value must be an integer literal, such as '0x02'");
                continue;
            }
            if (!integerFits(literal->value, literal->negative, storage)) {
                error(literal, "sema.literalOutOfRange", "the value " + literal->text + " doesn't fit in the storage type '" + storage->name + "'");
                continue;
            }
            remember(literal, storage);
            bits = literalBits(literal);
        }
        else {
            bool fits = nextValid && integerFits(next < 0 ? static_cast<uint64_t>(-(next + 1)) + 1 : static_cast<uint64_t>(next), next < 0, storage);
            if (!fits) {
                error(member->nameRange, "sema.literalOutOfRange",
                      "'" + member->name + "' would be one past the largest value of '" + storage->name + "'; give it an explicit value");
                continue;
            }
            bits = static_cast<uint64_t>(next);
        }
        if (const EnumMemberInfo* clash = info->memberWithValue(bits)) {
            error(member->nameRange, "sema.duplicateEnumValue", "'" + member->name + "' has the same value as '" + clash->name + "'");
            continue;
        }
        int64_t value = signedValue(bits, storage);
        if (storage->isSigned) {
            nextValid = value != INT64_MAX;
            next = value + 1;
        }
        else {
            nextValid = bits != UINT64_MAX;
            next = static_cast<int64_t>(bits + 1);
        }
        EnumMemberInfo memberInfo;
        memberInfo.name = member->name;
        memberInfo.value = storage->bits == 64 ? bits : bits & ((uint64_t(1) << storage->bits) - 1);
        memberInfo.ordinal = static_cast<uint32_t>(info->members.size());
        memberInfo.decl = member;
        info->dense = info->dense && memberInfo.value == memberInfo.ordinal;
        info->members.push_back(memberInfo);
    }
    if (info->members.empty()) {
        error(decl->nameRange, "sema.emptyEnum", "enum '" + info->name + "' needs at least one member");
        info->members.push_back({"<none>", 0, 0, nullptr});
    }
}

// ---------------------------------------------------------------------------
// named types and subranges

void Analyzer::declareNamedType(const TypeAliasDecl* decl)
{
    if (declaredHere(decl->name)) {
        error(decl->nameRange, "sema.redeclared", "'" + decl->name + "' is already declared");
        return;
    }
    auto info = std::make_unique<NamedTypeInfo>();
    info->name = decl->name;
    info->origin = currentOrigin();
    info->decl = decl;
    for (const Attribute* attribute : decl->attributes) {
        if (attribute->name == "alias" && attribute->value == nullptr) {
            info->isAlias = true;
        }
        else {
            unsupported(attribute, "the attribute '@[" + attribute->name + "]' on a type", "a later phase");
        }
    }
    model_.namedTypes.push_back(std::move(info));
}

const Type* Analyzer::resolveNamedType(NamedTypeInfo* info)
{
    if (info->type != nullptr) {
        return info->type;
    }
    if (info->resolving) {
        error(info->decl->nameRange, "sema.recursiveType", "type '" + info->name + "' is defined in terms of itself");
        info->type = types_.error();
        return info->type;
    }
    info->resolving = true;
    const TypeAliasDecl* decl = info->decl;
    const Type* type = types_.error();
    auto message = nodeCast<TypeMessage>(decl->type);
    if (message != nullptr && message->selector == "to:") {
        type = resolveSubrange(message->receiver, message->arguments[0], decl->storageType, info->name, decl->type);
        if (info->isAlias) {
            error(decl->nameRange, "sema.invalidAttribute", "a subrange is already interchangeable with its base; '@[alias]' doesn't apply");
        }
    }
    else {
        if (decl->storageType != nullptr) {
            error(decl->storageType, "sema.invalidStorage", "a storage type ('" + info->name + " : T') only applies to subranges");
        }
        const Type* base = resolveType(decl->type);
        if (base == types_.error() || rejectBareBlock(decl->type, base, "a named type")) {
            type = types_.error();
        }
        else if (info->isAlias) {
            type = base;
        }
        else if (isBlock(base) || isCFunction(base) || (isOptional(base) && (isBlock(base->inner) || isCFunction(base->inner)))) {
            // Blocks and C functions match by shape, so naming one just gives the shape a name.
            info->isAlias = true;
            type = base;
        }
        else if (isNumeric(base) || base == types_.boolType() || base->kind == TypeKind::character) {
            type = types_.distinct(base, info->name);
        }
        else {
            unsupported(decl->type, "a distinct named type of '" + typeName(base) + "' (use '@[alias]', or a record)", "a later phase");
        }
    }
    info->resolving = false;
    info->type = type;
    return type;
}

bool Analyzer::typeConstant(const Node* node, const Type*& type, int64_t& value)
{
    if (auto literal = nodeCast<IntegerLiteral>(node)) {
        if (!literal->negative && literal->value > static_cast<uint64_t>(INT64_MAX)) {
            unsupported(literal, "subrange bounds above the int64 range", "a later phase");
            return false;
        }
        value = literal->negative ? -static_cast<int64_t>(literal->value) : static_cast<int64_t>(literal->value);
        type = types_.int64();
        return true;
    }
    if (auto send = nodeCast<UnarySend>(node)) {
        auto receiver = nodeCast<Identifier>(send->receiver);
        EnumInfo* info = receiver != nullptr ? model_.enumNamed(receiver->name) : nullptr;
        if (info != nullptr) {
            resolveEnum(info);
            const EnumMemberInfo* member = info->member(send->selector);
            if (member == nullptr) {
                error(send->selectorRange, "sema.unknownMember", "'" + info->name + "' has no member '" + send->selector + "'");
                return false;
            }
            type = info->type;
            value = member->ordinal;
            return true;
        }
    }
    error(node, "sema.invalidSubrange", "subrange bounds must be integer literals or enum members, such as '0 to: 100' or 'Day mon to: Day fri'");
    return false;
}

const Type* Analyzer::resolveSubrange(const Node* lowNode, const Node* highNode, const Node* storageNode, const std::string& name,
                                      const Node* site)
{
    const Type* lowType = nullptr;
    const Type* highType = nullptr;
    int64_t low = 0;
    int64_t high = 0;
    if (!typeConstant(lowNode, lowType, low) || !typeConstant(highNode, highType, high)) {
        return types_.error();
    }
    if (lowType != highType) {
        error(site, "sema.invalidSubrange", "both bounds of a subrange must have the same type");
        return types_.error();
    }
    if (low > high) {
        error(site, "sema.emptySubrange", "the subrange is empty: its first value is greater than its last");
        return types_.error();
    }
    std::string label = name;
    if (isEnum(lowType)) {
        if (storageNode != nullptr) {
            error(storageNode, "sema.invalidStorage", "an enum subrange uses the enum's storage");
        }
        if (label.empty()) {
            const EnumInfo* info = lowType->enumInfo;
            label = info->name + " " + info->members[static_cast<size_t>(low)].name + " to: " + info->name + " " +
                    info->members[static_cast<size_t>(high)].name;
        }
        return types_.subrange(lowType, low, high, label);
    }
    auto fits = [](int64_t v, const Type* t) {
        return integerFits(v < 0 ? static_cast<uint64_t>(-(v + 1)) + 1 : static_cast<uint64_t>(v), v < 0, t);
    };
    const Type* base = nullptr;
    if (storageNode != nullptr) {
        base = resolveType(storageNode);
        if (!isInteger(base)) {
            if (base != types_.error()) {
                error(storageNode, "sema.invalidStorage", "a subrange's storage must be an integer type, not '" + typeName(base) + "'");
            }
            return types_.error();
        }
        if (!fits(low, base) || !fits(high, base)) {
            error(site, "sema.literalOutOfRange", "the subrange doesn't fit in '" + base->name + "'");
            return types_.error();
        }
    }
    else {
        for (uint32_t bits : {8u, 16u, 32u, 64u}) {
            const Type* candidate = types_.integer(bits, low < 0);
            if (fits(low, candidate) && fits(high, candidate)) {
                base = candidate;
                break;
            }
        }
    }
    if (label.empty()) {
        label = std::to_string(low) + " to: " + std::to_string(high);
    }
    return types_.subrange(base, low, high, label);
}

const Type* Analyzer::resolveTypeMessage(const TypeMessage* message, const std::string& name)
{
    auto receiver = nodeCast<TypeName>(message->receiver);
    std::string container = receiver != nullptr ? receiver->name : "";
    bool pointer = (container == "pointer" || container == "volatilePointer") && message->selector == "to:";
    if (message->selector == "to:" && !pointer) {
        return resolveSubrange(message->receiver, message->arguments[0], nullptr, name, message);
    }
    auto element = [&](const Node* node, const char* role) -> const Type* {
        const Type* type = resolveType(node);
        if (type == types_.error()) {
            return type;
        }
        // A block element must have its shape written out ('{ int32 [int32] }', not 'block').
        if (!isValueType(type) || type->kind == TypeKind::blockToInfer || (isBlock(type) && type->pending)) {
            error(node, "sema.invalidElementType", std::string("'") + typeName(type) + "' can't be " + role);
            return types_.error();
        }
        if (type->pending) {
            error(node, "sema.invalidElementType", "write the element type of a nested container in full, such as 'array of: int32'");
            return types_.error();
        }
        return type;
    };
    auto hashable = [&](const Node* node, const Type* type) {
        const Type* base = decay(type);
        bool ok = type == types_.error() || isInteger(base) || isEnum(base) || isString(base) || base == types_.boolType() ||
                  base->kind == TypeKind::character;
        if (!ok) {
            error(node, "sema.invalidKeyType", "'" + typeName(type) + "' can't be a key; use integers, enums, characters, bool or strings");
        }
        return ok;
    };
    // A dynamic container's buffer is a heap block, only 16-byte aligned.
    auto inBuffer = [&](const Node* node, const Type* type) {
        return type == types_.error() || fitsHeapAlignment(node, type, "a container's buffer") ? type : types_.error();
    };
    if (container == "array" && message->selector == "of:") {
        const Type* type = inBuffer(message->arguments[0], element(message->arguments[0], "an array element"));
        return type == types_.error() ? type : types_.arrayOf(type);
    }
    if (container == "array" && message->selector == "of:by:") {
        const Type* type = element(message->arguments[0], "an array element");
        const Type* index = resolveType(message->arguments[1]);
        if (type == types_.error() || index == types_.error()) {
            return types_.error();
        }
        if (isBlock(type) || isNonNullWord(type)) {
            error(message->arguments[0], "sema.invalidElementType",
                  "a fixed array's elements start out defaulted, and a " + std::string(isBlock(type) ? "block" : isPointer(type) ? "pointer" : "C function") +
                      " has no default; use 'array of: " + typeName(type) + "'" + (isNonNullWord(type) ? " or optional ones" : ""));
            return types_.error();
        }
        uint64_t count = 0;
        if (isEnum(index)) {
            count = index->enumInfo->members.size();
        }
        else if (isSubrange(index)) {
            count = static_cast<uint64_t>(index->high - index->low) + 1;
        }
        else {
            error(message->arguments[1], "sema.invalidIndexType", "a fixed array is indexed by an enum or a subrange such as '[0 to: 511]'");
            return types_.error();
        }
        if (count > (uint64_t(1) << 20)) {
            error(message, "sema.fixedArrayTooLarge", "a fixed array holds at most 1048576 elements; use a plain 'array of:'");
            return types_.error();
        }
        return types_.fixedArrayOf(type, index, count);
    }
    if (container == "dictionary" && message->selector == "of:") {
        const Type* type = inBuffer(message->arguments[0], element(message->arguments[0], "a dictionary value"));
        return type == types_.error() ? type : types_.dictionaryOf(type);
    }
    if (container == "hashtable" && message->selector == "keys:values:") {
        const Type* key = element(message->arguments[0], "a key");
        const Type* value = inBuffer(message->arguments[1], element(message->arguments[1], "a value"));
        if (key == types_.error() || value == types_.error() || !hashable(message->arguments[0], key)) {
            return types_.error();
        }
        return types_.hashtableOf(key, value);
    }
    if (pointer || (container == "slice" && message->selector == "of:")) {
        const Type* pointee = resolveType(message->arguments[0]);
        if (pointee == types_.error()) {
            return pointee;
        }
        if (!isPointee(pointee)) {
            error(message->arguments[0], "sema.invalidPointee",
                  "a pointer or slice points at plain data (numbers, characters, enums, records, fixed arrays, pointers), not a '" +
                      typeName(pointee) + "'; reference-counted values can't be reached through a pointer");
            return types_.error();
        }
        if (container == "slice") {
            return types_.sliceOf(canonical(pointee));
        }
        return types_.pointerTo(canonical(pointee), container == "volatilePointer");
    }
    if (container == "port" && message->selector == "of:") {
        const Type* data = resolveType(message->arguments[0]);
        if (data == types_.error()) {
            return data;
        }
        data = canonical(data);
        if (!isInteger(data) || data->isSigned || data->bits > 32 || data->distinctOf != nullptr) {
            error(message->arguments[0], "sema.invalidPortType",
                  "an I/O port moves a uint8, uint16 or uint32 ('in'/'out' have no other widths), not a '" + typeName(data) + "'");
            return types_.error();
        }
        return types_.portOf(data);
    }
    if (container == "set" && message->selector == "of:") {
        const Type* type = element(message->arguments[0], "a set element");
        if (type == types_.error() || !hashable(message->arguments[0], type)) {
            return types_.error();
        }
        return types_.setOf(type);
    }
    error(message, "sema.expectedType",
          "unknown type message '" + message->selector + "'; expected 'array of: T', 'array of: T by: I', 'dictionary of: V', "
          "'hashtable keys: K values: V', 'set of: T', 'pointer to: T', 'volatilePointer to: T', 'slice of: T', 'port of: T' or a subrange 'lo to: hi'");
    return types_.error();
}

// ---------------------------------------------------------------------------
// messages to types (`Color red`, `Percent fromValue: n`) and to values

bool Analyzer::constantBits(const Node* node, uint64_t& bits) const
{
    if (auto literal = nodeCast<IntegerLiteral>(node)) {
        bits = literalBits(literal);
        return true;
    }
    if (auto attributed = nodeCast<AttributedExpr>(node)) {
        return constantBits(attributed->expression, bits);
    }
    const SendInfo* send = model_.sendOf(node);
    if (send != nullptr && send->kind == SendKind::constant) {
        bits = send->constant;
        return true;
    }
    return false;
}

const Type* Analyzer::checkMetaUnary(const UnarySend* send, const Type* meta)
{
    const Type* instance = meta->instance;
    const std::string& selector = send->selector;
    auto constant = [&](const Type* type, uint64_t bits) {
        addSend(send, SendKind::constant, selector, type, type);
        model_.sends[send->id].constant = bits;
        return type;
    };
    if (isEnum(instance)) {
        const EnumInfo* info = instance->enumInfo;
        if (const EnumMemberInfo* member = info->member(selector)) {
            return constant(instance, member->value);
        }
        if (selector == "first") {
            return constant(instance, info->members.front().value);
        }
        if (selector == "last") {
            return constant(instance, info->members.back().value);
        }
        if (selector == "count") {
            return constant(types_.int32(), info->members.size());
        }
        error(send->selectorRange, "sema.unknownMember",
              "'" + info->name + "' has no member '" + selector + "' (members are always qualified: '" + info->name + " " +
                  info->members.front().name + "')");
        return types_.error();
    }
    if (isSubrange(instance)) {
        bool ordinal = isEnum(instance->inner);
        auto bits = [&](int64_t position) {
            return ordinal ? instance->enumInfo->members[static_cast<size_t>(position)].value : static_cast<uint64_t>(position);
        };
        if (selector == "first") {
            return constant(instance, bits(instance->low));
        }
        if (selector == "last") {
            return constant(instance, bits(instance->high));
        }
        if (selector == "count" || selector == "size") {
            return constant(types_.int64(), static_cast<uint64_t>(instance->high - instance->low) + 1);
        }
    }
    return nullptr;
}

const Type* Analyzer::checkMetaKeyword(const KeywordSend* send, const Type* meta, const Type* expected)
{
    (void)expected;
    const Type* instance = canonical(meta->instance);
    const std::string& selector = send->selector;
    if (isString(instance) || (instance != nullptr && instance->kind == TypeKind::character)) {
        return checkStringKeyword(send, meta, expected);
    }
    // `(set of: Perm) fromValue: reg`: `set of: T` in an expression names the type.
    if (selector == "of:" && instance != nullptr && instance->kind == TypeKind::set && instance->inner == nullptr) {
        const Type* elementMeta = check(send->arguments[0], nullptr);
        if (elementMeta->kind != TypeKind::meta || elementMeta->instance == nullptr || !isEnum(elementMeta->instance)) {
            if (elementMeta != types_.error()) {
                error(send->arguments[0], "sema.expectedType", "'set of:' in an expression takes an enum type");
            }
            return types_.error();
        }
        return types_.metaOf(types_.setOf(elementMeta->instance));
    }
    if (selector == "fromValue:") {
        const Node* argument = send->arguments[0];
        if (isEnum(instance) || (isSubrange(instance) && isEnum(instance->inner))) {
            const Type* storage = instance->enumInfo->storage;
            checkArgument(argument, storage);
            const Type* result = types_.optionalOf(instance);
            addSend(send, SendKind::enumFromValue, selector, instance, result);
            model_.sends[send->id].enumInfo = instance->enumInfo;
            return result;
        }
        if (isSubrange(instance)) {
            const Type* value = check(argument, instance->inner);
            if (value != types_.error() && !isInteger(decay(value))) {
                error(argument, "sema.typeMismatch", "'fromValue:' takes an integer, not '" + typeName(value) + "'");
                return types_.error();
            }
            const Type* result = types_.optionalOf(instance);
            addSend(send, SendKind::subrangeFromValue, selector, decay(value), result);
            return result;
        }
        if (instance != nullptr && instance->distinctOf != nullptr) {
            checkArgument(argument, instance->distinctOf);
            addSend(send, SendKind::distinctFromValue, selector, instance->distinctOf, instance);
            return instance;
        }
        if (isBitSet(instance)) {
            const Type* raw = types_.integer(instance->bits, false);
            checkArgument(argument, raw);
            const Type* result = types_.optionalOf(instance);
            addSend(send, SendKind::bitSetFromValue, selector, instance, result);
            return result;
        }
    }
    if (selector == "do:" && (isEnum(instance) || isSubrange(instance))) {
        if (!checkInlinedEach(send->arguments[0], "do:", instance, 1)) {
            return types_.error();
        }
        addSend(send, SendKind::inlineEnumDo, selector, instance, types_.voidType());
        return types_.voidType();
    }
    return nullptr;
}

const Type* Analyzer::checkValueUnary(const UnarySend* send, const Type* receiver)
{
    const std::string& selector = send->selector;
    if (isOrdinal(receiver)) {
        const EnumInfo* info = receiver->enumInfo;
        SendKind kind = SendKind::constant;
        const Type* result = nullptr;
        if (selector == "value") {
            kind = SendKind::enumValue;
            result = info->storage;
        }
        else if (selector == "ordinal") {
            kind = SendKind::enumOrdinal;
            result = types_.int32();
        }
        else if (selector == "succ" || selector == "pred") {
            kind = selector == "succ" ? SendKind::enumSucc : SendKind::enumPred;
            result = receiver;
        }
        else if (selector == "name") {
            kind = SendKind::enumName;
            result = types_.string();
        }
        if (result != nullptr) {
            addSend(send, kind, selector, receiver, result);
            model_.sends[send->id].enumInfo = info;
            return result;
        }
        return nullptr;
    }
    if (receiver->distinctOf != nullptr && selector == "value") {
        addSend(send, SendKind::distinctValue, selector, receiver, receiver->distinctOf);
        return receiver->distinctOf;
    }
    if (isSubrange(receiver) && selector == "value") {
        addSend(send, SendKind::distinctValue, selector, receiver, receiver->inner);
        return receiver->inner;
    }
    if (isBitSet(receiver)) {
        if (selector == "count") {
            addSend(send, SendKind::bitSetCount, selector, receiver, types_.int32());
            return types_.int32();
        }
        if (selector == "value") {
            const Type* raw = types_.integer(receiver->bits, false);
            addSend(send, SendKind::bitSetValue, selector, receiver, raw);
            return raw;
        }
        if (selector == "isEmpty") {
            addSend(send, SendKind::bitSetIsEmpty, selector, receiver, types_.boolType());
            return types_.boolType();
        }
    }
    return nullptr;
}

bool Analyzer::isAssignableTarget(const Node* node) const
{
    if (auto identifier = nodeCast<Identifier>(node)) {
        if (model_.selfFieldRefs.count(identifier->id) != 0) {
            return true;
        }
        const LocalInfo* local = model_.localOf(identifier);
        return local != nullptr && !local->readOnly;
    }
    if (auto access = nodeCast<FieldAccess>(node)) {
        if (isClass(model_.typeOf(access->receiver))) {
            return true;
        }
        const LocalInfo* root = rootLocal(access);
        return root != nullptr && !root->readOnly;
    }
    return false;
}

const Type* Analyzer::checkValueKeyword(const KeywordSend* send, const Type* receiver, const Type* expected)
{
    (void)expected;
    const std::string& selector = send->selector;
    if (isBitSet(receiver)) {
        const Type* element = receiver->inner;
        if (selector == "includes:") {
            checkArgument(send->arguments[0], element);
            addSend(send, SendKind::bitSetIncludes, selector, receiver, types_.boolType());
            return types_.boolType();
        }
        if (selector == "add:" || selector == "remove:") {
            checkArgument(send->arguments[0], element);
            if (!isAssignableTarget(send->receiver)) {
                error(send->receiver, "sema.notAssignable", "'" + selector + "' changes the set, so it needs a variable or field");
            }
            addSend(send, SendKind::bitSetMutate, selector, receiver, types_.voidType());
            return types_.voidType();
        }
    }
    if (isOrdinal(receiver) && selector == "to:") {
        check(send->arguments[0], nullptr);
        error(send, "sema.intervalOutsideFor", "an interval 'a to: b' can only be the iterable of a 'for' loop for now");
        return types_.error();
    }
    return nullptr;
}

const Type* Analyzer::checkBitSetBinary(const BinarySend* send, const Type* left)
{
    const std::string& op = send->selector;
    const Type* right = check(send->argument, left);
    if (right == types_.error()) {
        return types_.error();
    }
    if (canonical(right) != canonical(left)) {
        error(send, "sema.typeMismatch", "'" + op + "' needs two '" + typeName(left) + "' values, not '" + typeName(right) + "'");
        return types_.error();
    }
    if (op == "+" || op == "-" || op == "*") {
        addSend(send, SendKind::bitSetOp, op, left, left);
        return left;
    }
    if (op == "=" || op == "<>") {
        addSend(send, SendKind::comparison, op, left, types_.boolType());
        return types_.boolType();
    }
    error(send->selectorRange, "sema.unknownMessage",
          "sets support '+' (union), '-' (difference), '*' (intersection), '=' and '<>', not '" + op + "'");
    return types_.error();
}

const Type* Analyzer::checkOptionalComparison(const BinarySend* send, const Type* left)
{
    return checkReferenceComparison(send, left);
}

// ---------------------------------------------------------------------------
// array literals as enum sets (other literals are in analyzerContainers.cpp)

const Type* Analyzer::checkOrdinalIterable(const Node* iterable)
{
    if (auto identifier = nodeCast<Identifier>(iterable)) {
        if (lookupLocal(identifier->name) != nullptr || selfField(identifier->name) != nullptr) {
            return nullptr;
        }
        const Type* meta = check(iterable, nullptr);
        if (meta->kind == TypeKind::meta && meta->instance != nullptr && (isEnum(meta->instance) || isSubrange(meta->instance))) {
            addSend(iterable, SendKind::enumRange, "", meta->instance, meta->instance);
            return meta->instance;
        }
        if (meta != types_.error()) {
            error(iterable, "sema.notIterable", "'" + typeName(meta) + "' can't be iterated; use an enum, a subrange or 'a to: b'");
        }
        return types_.error();
    }
    return nullptr;
}

bool Analyzer::isCheckedConversion(const Type* from, const Type* to) const
{
    from = canonical(from);
    to = canonical(to);
    if (!isSubrange(to) || from == nullptr) {
        return false;
    }
    if (isInteger(to->inner)) {
        return isInteger(decay(from));
    }
    return decay(from) == to->inner;
}

// `case` over an enum, an enum subrange or an integer subrange: every value can
// be listed, so without `else` every one must be covered.
void Analyzer::checkEnumCase(const CaseStmt* statement, const Type* subject)
{
    const Type* base = decay(subject);
    bool ordinal = isEnum(base);
    const EnumInfo* info = ordinal ? base->enumInfo : nullptr;
    int64_t domainLow = isSubrange(subject) ? subject->low : 0;
    int64_t domainHigh = isSubrange(subject) ? subject->high : static_cast<int64_t>(info->members.size()) - 1;
    std::vector<std::pair<int64_t, int64_t>> covered;

    // A label's position: the ordinal for enums, the value for integer subranges.
    auto position = [&](const Node* label, int64_t& out) {
        const Type* type = check(label, ordinal ? base : subject);
        if (type == types_.error()) {
            return false;
        }
        uint64_t bits = 0;
        if (!constantBits(label, bits)) {
            error(label, "sema.caseLabelNotConstant",
                  ordinal ? "case labels must be members such as '" + info->name + " " + info->members.front().name + "' or ranges"
                          : "case labels must be integer literals or ranges such as '(1 to: 9)'");
            return false;
        }
        if (ordinal) {
            if (decay(type) != base) {
                error(label, "sema.typeMismatch", "this label is a '" + typeName(type) + "', but the case is over '" + typeName(subject) + "'");
                return false;
            }
            const EnumMemberInfo* member = info->memberWithValue(bits);
            out = member != nullptr ? member->ordinal : 0;
        }
        else {
            out = signedValue(bits, base);
        }
        if (out < domainLow || out > domainHigh) {
            error(label, "sema.caseLabelOutOfRange", "this label is outside '" + typeName(subject) + "'");
            return false;
        }
        return true;
    };
    auto toBits = [&](int64_t p) {
        return ordinal ? info->members[static_cast<size_t>(p)].value : static_cast<uint64_t>(p);
    };

    for (const Node* node : statement->entries) {
        auto entry = nodeCast<CaseEntry>(node);
        if (entry == nullptr) {
            continue;
        }
        std::vector<CaseRange> ranges;
        for (const Node* label : entry->labels) {
            int64_t low = 0;
            int64_t high = 0;
            auto interval = nodeCast<KeywordSend>(label);
            if (interval != nullptr && interval->selector == "to:") {
                if (!position(interval->receiver, low) || !position(interval->arguments[0], high)) {
                    continue;
                }
                if (low > high) {
                    error(label, "sema.emptyCaseRange", "the case range is empty: its first value is greater than its last");
                    continue;
                }
                remember(interval, subject);
            }
            else {
                if (!position(label, low)) {
                    continue;
                }
                high = low;
            }
            bool clash = false;
            for (const auto& [a, b] : covered) {
                clash = clash || (low <= b && a <= high);
            }
            if (clash) {
                error(label, "sema.overlappingCaseLabels", "this case label overlaps an earlier one");
                continue;
            }
            covered.push_back({low, high});
            if (ordinal) {
                for (int64_t p = low; p <= high; ++p) {
                    ranges.push_back({toBits(p), toBits(p)});
                }
            }
            else {
                ranges.push_back({toBits(low), toBits(high)});
            }
        }
        model_.caseRanges[entry->id] = ranges;
        auto body = nodeCast<BlockLiteral>(entry->body);
        if (body != nullptr) {
            if (!body->params.empty()) {
                error(body->params.front(), "sema.invalidCaseBinding", "a case entry doesn't bind a value; use the subject itself");
            }
            pushScope();
            checkStatements(body->statements);
            popScope();
        }
    }

    std::sort(covered.begin(), covered.end());
    std::vector<std::string> missing;
    int64_t next = domainLow;
    bool complete = true;
    for (const auto& [low, high] : covered) {
        if (low > next) {
            complete = false;
            for (int64_t p = next; p < low && ordinal && missing.size() < 5; ++p) {
                missing.push_back(info->members[static_cast<size_t>(p)].name);
            }
        }
        next = std::max(next, high + 1);
    }
    if (next <= domainHigh) {
        complete = false;
        for (int64_t p = next; p <= domainHigh && ordinal && missing.size() < 5; ++p) {
            missing.push_back(info->members[static_cast<size_t>(p)].name);
        }
    }
    if (complete) {
        model_.exhaustiveCases.insert(statement->id);
    }
    if (statement->elseBody != nullptr) {
        checkGroup(statement->elseBody);
    }
    else if (!complete) {
        std::string list;
        for (const std::string& name : missing) {
            list += (list.empty() ? "" : ", ") + name;
        }
        error(statement, "sema.caseNotExhaustive",
              "this 'case' doesn't cover every value of '" + typeName(subject) + "'" +
                  (list.empty() ? std::string() : " (missing: " + list + ")") + "; add the missing labels or an 'else'");
    }
}

}

namespace bob::sema::detail {

const Type* Analyzer::literalExpectation(const Type* expected, bool isRealLiteral) const
{
    expected = canonical(expected);
    (void)isRealLiteral;
    if (isOptional(expected)) {
        expected = expected->inner;
    }
    return expected;
}

}
