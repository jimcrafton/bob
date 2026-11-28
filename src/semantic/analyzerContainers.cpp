// Strings, built-in containers and element-type inference (phase 5).

#include "analyzerInternal.h"

#include <algorithm>

namespace bob::sema::detail {

namespace {

const char* viewName(const Type* view)
{
    static const char* names[] = {"bytes", "codepoints", "graphemes"};
    return names[view->count];
}

// Values `=` can compare, which `includes:` and hash keys need.
bool isEquatable(const Type* type)
{
    const Type* base = decay(canonical(type));
    return isInteger(base) || isEnum(base) || isString(base) || base->kind == TypeKind::boolType || base->kind == TypeKind::character;
}

bool isPrintable(const Type* type)
{
    type = decay(canonical(type));
    return isNumeric(type) || isEnum(type) || isString(type) || type->kind == TypeKind::boolType || type->kind == TypeKind::character;
}

}

// ---------------------------------------------------------------------------
// element-type inference

const Type* Analyzer::elementOf(const Type* container, const Node* site, bool required)
{
    const Type* type = canonical(container);
    if (type->pending) {
        if (required) {
            error(site, "sema.cannotInferElement",
                  "the element type of this '" + type->name + "' isn't known yet; add an element first or declare it as '" +
                      (type->kind == TypeKind::hashtable ? std::string("hashtable keys: K values: V") : type->name + " of: T") + "'");
        }
        return nullptr;
    }
    return type->inner;
}

bool Analyzer::settleContainer(const Type* pendingType, const Type* key, const Type* element, const Node* site)
{
    const Type* current = canonical(pendingType);
    if (element == types_.error() || key == types_.error()) {
        return false;
    }
    if (!current->pending) {
        return true;
    }
    auto valid = [&](const Type* type, const char* role) {
        type = canonical(type);
        if (!isValueType(type) || type->pending) {
            bool block = isBlock(type);
            error(site, block ? "sema.cannotInferBlock" : "sema.invalidElementType",
                  block          ? std::string("a block stored in a container needs its parameter types; write them, as in '{ [x: int32] … }'")
                  : type->pending ? std::string("a nested container needs its element type written out, such as 'array of: int32'")
                                  : "'" + typeName(type) + "' can't be " + role);
            return false;
        }
        return fitsHeapAlignment(site, type, "a container's buffer");
    };
    const Type* concrete = nullptr;
    switch (current->kind) {
    case TypeKind::array:
        if (!valid(element, "an array element")) {
            return false;
        }
        concrete = types_.arrayOf(canonical(element));
        break;
    case TypeKind::dictionary:
        if (!valid(element, "a dictionary value")) {
            return false;
        }
        concrete = types_.dictionaryOf(canonical(element));
        break;
    case TypeKind::hashtable:
        if (!valid(element, "a value") || !valid(key, "a key")) {
            return false;
        }
        if (!isEquatable(key)) {
            error(site, "sema.invalidKeyType", "'" + typeName(key) + "' can't be a key; use integers, enums, characters, bool or strings");
            return false;
        }
        concrete = types_.hashtableOf(canonical(key), canonical(element));
        break;
    case TypeKind::set:
        if (!valid(element, "a set element")) {
            return false;
        }
        if (!isEquatable(element)) {
            error(site, "sema.invalidKeyType", "'" + typeName(element) + "' can't be a set element; use integers, enums, characters, bool or strings");
            return false;
        }
        concrete = types_.setOf(canonical(element));
        break;
    default:
        return false;
    }
    const_cast<Type*>(current)->resolved = concrete;
    return true;
}

bool Analyzer::unifyContainer(const Type* pendingType, const Type* concreteType, const Node* site)
{
    const Type* pending = canonical(pendingType);
    const Type* concrete = canonical(concreteType);
    if (pending == concrete) {
        return true;
    }
    if (concrete->pending) {
        const_cast<Type*>(pending)->resolved = concrete;
        return true;
    }
    if (!settleContainer(pending, concrete->key, concrete->inner, site)) {
        return false;
    }
    if (canonical(pending) != concrete) {
        error(site, "sema.typeMismatch", "this is a '" + typeName(concrete) + "', but '" + typeName(pending) + "' is expected");
        return false;
    }
    return true;
}

void Analyzer::finalizePendingContainers()
{
    auto report = [&](const Type* type, const Node* where, const std::string& what) {
        if (type == nullptr || where == nullptr) {
            return;
        }
        const Type* c = canonical(type);
        if (c->pending && c->kind != TypeKind::block) {
            error(where, "sema.cannotInferElement",
                  "the element type of " + what + " can't be inferred; nothing is ever stored into it, so declare it as '" +
                      (c->kind == TypeKind::hashtable ? std::string("hashtable keys: K values: V") : c->name + " of: T") + "'");
        }
    };
    for (const auto& local : model_.locals) {
        report(local->type, local->decl, "'" + local->name + "'");
    }
    for (const auto& info : model_.classes) {
        for (const FieldInfo& field : info->fields) {
            if (field.owner == info.get()) {
                report(field.type, field.decl, "field '" + field.name + "'");
            }
        }
    }
    for (const auto& message : model_.messages) {
        if (message->decl != nullptr && message->decl->returnType != nullptr) {
            report(message->returnType, message->decl->returnType, "the result of '" + message->selector + "'");
        }
    }
}

void Analyzer::requireMutable(const Node* receiver, const std::string& selector)
{
    if (!isAssignableTarget(receiver)) {
        error(receiver, "sema.notAssignable", "'" + selector + "' changes the container, so its receiver must be a variable or field");
    }
}

const Type* Analyzer::mergeElementTypes(const std::vector<const Type*>& elementTypes, const Node* site)
{
    std::vector<const Type*> types;
    bool sawNil = false;
    for (const Type* type : elementTypes) {
        type = canonical(type);
        if (type == types_.error()) {
            return types_.error();
        }
        if (type->kind == TypeKind::nilType) {
            sawNil = true;
            continue;
        }
        if (!isValueType(type) || type->pending) {
            error(site, "sema.invalidElementType", "'" + typeName(type) + "' can't be an element of a literal");
            return types_.error();
        }
        types.push_back(type);
    }
    if (types.empty()) {
        error(site, "sema.cannotInfer", "can't infer an element type from nil alone; declare the container");
        return types_.error();
    }
    const Type* common = types.front();
    for (size_t i = 1; i < types.size() && common != nullptr; ++i) {
        common = commonType(common, types[i]);
    }
    if (common == nullptr && std::all_of(types.begin(), types.end(), [](const Type* t) { return isClass(t); })) {
        // The closest class or protocol every element is a kind of.
        std::vector<ClassInfo*> candidates;
        for (ClassInfo* info = types.front()->classInfo; info != nullptr; info = info->superclass) {
            candidates.push_back(info);
        }
        std::vector<ClassInfo*> pending{types.front()->classInfo};
        while (!pending.empty()) {
            ClassInfo* info = pending.back();
            pending.pop_back();
            for (ClassInfo* protocol : info->protocols) {
                candidates.push_back(protocol);
                pending.push_back(protocol);
            }
            if (info->superclass != nullptr) {
                pending.push_back(info->superclass);
            }
        }
        for (ClassInfo* candidate : candidates) {
            if (std::all_of(types.begin(), types.end(), [&](const Type* t) { return t->classInfo->isKindOf(candidate); })) {
                common = candidate->type;
                break;
            }
        }
    }
    if (common == nullptr) {
        std::string names;
        for (size_t i = 0; i < types.size(); ++i) {
            if (names.find("'" + typeName(types[i]) + "'") == std::string::npos) {
                names += (names.empty() ? "" : ", ") + ("'" + typeName(types[i]) + "'");
            }
        }
        error(site, "sema.mixedElementTypes",
              "these elements have no common type (" + names +
                  "); every element must convert to one type, or be objects sharing a class or protocol");
        return types_.error();
    }
    if (sawNil) {
        if (isOptional(common)) {
            error(site, "sema.invalidElementType", "nil can't be mixed into '" + typeName(common) + "'");
            return types_.error();
        }
        common = types_.optionalOf(common);
    }
    return common;
}

// ---------------------------------------------------------------------------
// literals

const Type* Analyzer::checkArrayLiteral(const ArrayLiteral* literal, const Type* expected)
{
    const Type* want = canonical(expected);
    auto checkAll = [&](const Type* element) {
        for (const Node* node : literal->elements) {
            checkArgument(node, element);
        }
    };
    if (isBitSet(want)) {
        checkAll(want->inner);
        addSend(literal, SendKind::bitSetLiteral, "", want, want);
        return want;
    }
    if (want != nullptr && want->kind == TypeKind::fixedArray) {
        if (literal->elements.size() != want->count) {
            error(literal, "sema.wrongElementCount",
                  "a '" + want->name + "' has " + std::to_string(want->count) + " elements, but this literal has " +
                      std::to_string(literal->elements.size()));
        }
        checkAll(want->inner);
        addSend(literal, SendKind::fixedArrayLiteral, "", want, want);
        return want;
    }
    bool wantsSet = want != nullptr && want->kind == TypeKind::set;
    if (want != nullptr && (want->kind == TypeKind::array || wantsSet) && !want->pending) {
        checkAll(want->inner);
        addSend(literal, wantsSet ? SendKind::setLiteral : SendKind::arrayLiteral, "", want, want);
        return want;
    }

    // Infer the element type from the elements.
    if (literal->elements.empty()) {
        const Type* type = want != nullptr && want->pending && (want->kind == TypeKind::array || wantsSet)
                               ? want
                               : types_.pendingContainer(TypeKind::array);
        addSend(literal, wantsSet ? SendKind::setLiteral : SendKind::arrayLiteral, "", type, type);
        return type;
    }
    const Node* anchorNode = nullptr;
    bool anyReal = false;
    for (const Node* node : literal->elements) {
        if (!isUntypedLiteral(node)) {
            anchorNode = anchorNode == nullptr ? node : anchorNode;
        }
        anyReal = anyReal || node->kind == NodeKind::realLiteral;
    }
    const Type* anchor = anchorNode != nullptr ? check(anchorNode, nullptr) : anyReal ? types_.real64() : nullptr;
    std::vector<const Type*> elementTypes;
    for (const Node* node : literal->elements) {
        const Type* hint = isNumeric(decay(anchor)) ? decay(anchor) : nullptr;
        if (isUntypedLiteral(node) && node->kind == NodeKind::realLiteral && !isReal(hint)) {
            hint = nullptr;
        }
        elementTypes.push_back(node == anchorNode ? anchor : check(node, hint));
    }
    const Type* element = mergeElementTypes(elementTypes, literal);
    if (element == types_.error()) {
        return types_.error();
    }
    if (wantsSet) {
        const Type* type = types_.setOf(element);
        if (!isEquatable(element)) {
            error(literal, "sema.invalidKeyType", "'" + typeName(element) + "' can't be a set element");
            return types_.error();
        }
        addSend(literal, isBitSet(type) ? SendKind::bitSetLiteral : SendKind::setLiteral, "", type, type);
        return type;
    }
    const Type* type = types_.arrayOf(element);
    addSend(literal, SendKind::arrayLiteral, "", type, type);
    return type;
}

// ---------------------------------------------------------------------------
// iteration

const Type* Analyzer::checkCollectionIterable(const ForStmt* statement)
{
    const Node* iterable = statement->iterable;
    const Type* type = canonical(check(iterable, nullptr));
    if (type == types_.error()) {
        return type;
    }
    const Type* element = nullptr;
    if (isStringView(type) || type->kind == TypeKind::slice) {
        element = type->inner;
    }
    else if (type->kind == TypeKind::array || type->kind == TypeKind::fixedArray || type->kind == TypeKind::set) {
        element = elementOf(type, iterable, true);
        if (element == nullptr) {
            return types_.error();
        }
    }
    else if (isString(type)) {
        error(iterable, "sema.notIterable", "a string has no single length; iterate a view: 's bytes', 's codepoints' or 's graphemes'");
        return types_.error();
    }
    else if (type->kind == TypeKind::dictionary || type->kind == TypeKind::hashtable) {
        error(iterable, "sema.notIterable", "iterate 'd keys' or 'd values', or use 'd keysAndValuesDo: { [k, v] ... }'");
        return types_.error();
    }
    else {
        error(iterable, "sema.notIterable",
              "'" + typeName(type) + "' can't be iterated; use an interval 'a to: b', an enum, a subrange or a collection");
        return types_.error();
    }
    addSend(statement, SendKind::collectionRange, "", type, element);
    return element;
}

// ---------------------------------------------------------------------------
// strings and characters

const Type* Analyzer::checkStringUnary(const UnarySend* send, const Type* receiver)
{
    const std::string& selector = send->selector;
    if (selector == "printString") {
        if (!isPrintable(receiver)) {
            return nullptr;
        }
        addSend(send, SendKind::printString, selector, receiver, types_.string());
        return types_.string();
    }
    if (isString(receiver)) {
        int view = selector == "bytes" ? 0 : selector == "codepoints" ? 1 : selector == "graphemes" ? 2 : -1;
        if (view >= 0) {
            const Type* type = types_.stringView(view);
            addSend(send, SendKind::stringView, selector, receiver, type);
            return type;
        }
        if (selector == "isEmpty") {
            addSend(send, SendKind::stringIsEmpty, selector, receiver, types_.boolType());
            return types_.boolType();
        }
        if (selector == "toUtf8") {
            const Type* type = types_.arrayOf(types_.uint8());
            addSend(send, SendKind::stringToUtf8, selector, receiver, type);
            return type;
        }
        if (selector == "size" || selector == "length" || selector == "count") {
            error(send->selectorRange, "sema.unknownMessage",
                  "a string has no single '" + selector + "'; ask a view: 's bytes count', 's codepoints count' or 's graphemes count'");
            return types_.error();
        }
    }
    if (isStringView(receiver) && selector == "count") {
        addSend(send, SendKind::stringViewCount, viewName(receiver), receiver, types_.int64());
        return types_.int64();
    }
    if (receiver->kind == TypeKind::character) {
        if (selector == "value") {
            const Type* type = types_.integer(32, false);
            addSend(send, SendKind::charValue, selector, receiver, type);
            return type;
        }
        if (selector == "asString") {
            addSend(send, SendKind::stringFromChar, selector, receiver, types_.string());
            return types_.string();
        }
    }
    return nullptr;
}

const Type* Analyzer::checkStringKeyword(const KeywordSend* send, const Type* receiver, const Type* expected)
{
    (void)expected;
    const std::string& selector = send->selector;
    const Node* argument = send->arguments.empty() ? nullptr : send->arguments[0];
    if (receiver->kind == TypeKind::meta) {
        const Type* instance = receiver->instance;
        if (isString(instance) && (selector == "fromUtf8:" || selector == "fromUtf8Lossy:")) {
            checkArgument(argument, types_.arrayOf(types_.uint8()));
            bool lossy = selector == "fromUtf8Lossy:";
            const Type* result = lossy ? types_.string() : types_.optionalOf(types_.string());
            addSend(send, lossy ? SendKind::stringFromUtf8Lossy : SendKind::stringFromUtf8, selector, instance, result);
            return result;
        }
        if (instance->kind == TypeKind::character && selector == "fromValue:") {
            checkArgument(argument, types_.integer(32, false));
            const Type* result = types_.optionalOf(instance);
            addSend(send, SendKind::charFromValue, selector, instance, result);
            return result;
        }
        return nullptr;
    }
    if (isStringView(receiver)) {
        if (selector == "at:" && receiver->count == 0) {
            const Type* index = check(argument, types_.int64());
            if (index != types_.error() && !isInteger(decay(index))) {
                error(argument, "sema.typeMismatch", "a byte index is an integer, not '" + typeName(index) + "'");
                return types_.error();
            }
            addSend(send, SendKind::stringByteAt, selector, decay(index), types_.uint8());
            return types_.uint8();
        }
        if (selector == "at:") {
            check(argument, nullptr);
            error(send, "sema.unknownMessage",
                  std::string("'s ") + viewName(receiver) + "' has no integer 'at:'; only 's bytes at: n' is indexed by number");
            return types_.error();
        }
        if (selector == "do:") {
            if (!checkInlinedEach(argument, "do:", receiver->inner, 1)) {
                return types_.error();
            }
            addSend(send, SendKind::inlineDo, selector, receiver, types_.voidType());
            return types_.voidType();
        }
        return nullptr;
    }
    if (isString(receiver)) {
        if (selector == "startsWith:" || selector == "endsWith:" || selector == "includesSubstring:") {
            checkArgument(argument, types_.string());
            addSend(send, SendKind::stringPredicate, selector, receiver, types_.boolType());
            return types_.boolType();
        }
        if (selector == "split:") {
            checkArgument(argument, types_.string());
            const Type* result = types_.arrayOf(types_.string());
            addSend(send, SendKind::stringSplit, selector, receiver, result);
            return result;
        }
        if (selector == "join:") {
            checkArgument(argument, types_.arrayOf(types_.string()));
            addSend(send, SendKind::stringJoin, selector, receiver, types_.string());
            return types_.string();
        }
    }
    return nullptr;
}

const Type* Analyzer::checkStringBinary(const BinarySend* send, const Type* left)
{
    const std::string& op = send->selector;
    if (isString(left)) {
        const Type* right = check(send->argument, types_.string());
        if (right == types_.error()) {
            return types_.error();
        }
        if (!isString(right)) {
            error(send, "sema.typeMismatch",
                  "'" + op + "' needs two strings, not '" + typeName(right) + "'" + (isPrintable(right) ? "; convert it with 'printString'" : ""));
            return types_.error();
        }
        if (op == "+") {
            addSend(send, SendKind::stringConcat, op, left, types_.string());
            return types_.string();
        }
        if (isComparisonOperator(op)) {
            addSend(send, SendKind::stringCompare, op, left, types_.boolType());
            return types_.boolType();
        }
        error(send->selectorRange, "sema.unknownMessage", "strings support '+' and comparisons, not '" + op + "'");
        return types_.error();
    }
    if (left->kind == TypeKind::character) {
        const Type* right = check(send->argument, left);
        if (right == types_.error()) {
            return types_.error();
        }
        if (right->kind != TypeKind::character || !isComparisonOperator(op)) {
            error(send, "sema.typeMismatch", "characters compare only with characters ('" + op + "' with '" + typeName(right) + "')");
            return types_.error();
        }
        addSend(send, SendKind::comparison, op, left, types_.boolType());
        return types_.boolType();
    }
    check(send->argument, nullptr);
    error(send->selectorRange, "sema.unknownMessage", "'" + typeName(left) + "' doesn't understand '" + op + "'");
    return types_.error();
}

// ---------------------------------------------------------------------------
// containers

const Type* Analyzer::checkContainerUnary(const UnarySend* send, const Type* receiver)
{
    const std::string& selector = send->selector;
    const Type* type = canonical(receiver);
    if (selector == "count") {
        addSend(send, SendKind::containerCount, selector, type, types_.int64());
        return types_.int64();
    }
    if (selector == "isEmpty") {
        addSend(send, SendKind::containerIsEmpty, selector, type, types_.boolType());
        return types_.boolType();
    }
    if (type->kind == TypeKind::array && (selector == "first" || selector == "last" || selector == "removeLast")) {
        const Type* element = elementOf(type, send, true);
        if (element == nullptr) {
            return types_.error();
        }
        if (selector == "removeLast") {
            requireMutable(send->receiver, selector);
        }
        SendKind kind = selector == "first" ? SendKind::arrayFirst : selector == "last" ? SendKind::arrayLast : SendKind::arrayRemoveLast;
        addSend(send, kind, selector, type, element);
        return element;
    }
    if ((type->kind == TypeKind::dictionary || type->kind == TypeKind::hashtable) && (selector == "keys" || selector == "values")) {
        const Type* value = elementOf(type, send, true);
        if (value == nullptr) {
            return types_.error();
        }
        const Type* result = types_.arrayOf(selector == "keys" ? type->key : value);
        addSend(send, selector == "keys" ? SendKind::dictionaryKeys : SendKind::dictionaryValues, selector, type, result);
        return result;
    }
    return nullptr;
}

const Type* Analyzer::checkContainerKeyword(const KeywordSend* send, const Type* receiver, const Type* expected)
{
    (void)expected;
    const std::string& selector = send->selector;
    const Type* type = canonical(receiver);
    const auto& args = send->arguments;
    auto integerIndex = [&](const Node* node) {
        const Type* index = check(node, types_.int64());
        if (index != types_.error() && !isInteger(decay(index))) {
            error(node, "sema.typeMismatch", "an array index is an integer, not '" + typeName(index) + "'");
            return false;
        }
        return index != types_.error();
    };
    auto record = [&](SendKind kind, const Type* result) {
        addSend(send, kind, selector, receiver, result);
        return result;
    };

    if (type->kind == TypeKind::array) {
        if (selector == "at:") {
            const Type* element = elementOf(type, send, true);
            if (!integerIndex(args[0]) || element == nullptr) {
                return types_.error();
            }
            return record(SendKind::containerAt, element);
        }
        if (selector == "at:put:" || selector == "add:") {
            const Node* valueNode = args.back();
            if (selector == "at:put:") {
                integerIndex(args[0]);
            }
            requireMutable(send->receiver, selector);
            const Type* element = elementOf(type, send, false);
            if (element == nullptr) {
                const Type* value = check(valueNode, nullptr);
                if (value == types_.error()) {
                    return value;
                }
                if (value->kind == TypeKind::nilType) {
                    error(valueNode, "sema.cannotInfer", "can't infer an element type from nil; declare the array as 'array of: T?'");
                    return types_.error();
                }
                settleContainer(type, nullptr, value, valueNode);
            }
            else {
                checkArgument(valueNode, element);
            }
            return record(selector == "add:" ? SendKind::containerAdd : SendKind::containerAtPut, types_.voidType());
        }
        if (selector == "includes:") {
            const Type* element = elementOf(type, send, true);
            if (element == nullptr) {
                return types_.error();
            }
            if (!isEquatable(element)) {
                error(send, "sema.unknownMessage", "'includes:' needs elements that '=' can compare, not '" + typeName(element) + "'");
                return types_.error();
            }
            checkArgument(args[0], element);
            return record(SendKind::containerIncludes, types_.boolType());
        }
    }
    if (type->kind == TypeKind::fixedArray) {
        if (selector == "at:") {
            checkArgument(args[0], type->key);
            return record(SendKind::containerAt, type->inner);
        }
        if (selector == "at:put:") {
            checkArgument(args[0], type->key);
            checkArgument(args[1], type->inner);
            requireMutable(send->receiver, selector);
            return record(SendKind::containerAtPut, types_.voidType());
        }
    }
    if (type->kind == TypeKind::dictionary || type->kind == TypeKind::hashtable) {
        bool isDictionary = type->kind == TypeKind::dictionary;
        const Type* keyType = isDictionary ? types_.string() : (type->pending ? nullptr : type->key);
        auto checkKey = [&](const Node* node) {
            if (keyType != nullptr) {
                checkArgument(node, keyType);
                return keyType;
            }
            return check(node, nullptr);
        };
        if (selector == "at:put:") {
            requireMutable(send->receiver, selector);
            const Type* key = checkKey(args[0]);
            const Type* value = elementOf(type, send, false);
            if (value == nullptr) {
                const Type* given = check(args[1], nullptr);
                settleContainer(type, key, given, args[1]);
            }
            else {
                checkArgument(args[1], value);
            }
            return record(SendKind::containerAtPut, types_.voidType());
        }
        const Type* value = elementOf(type, send, true);
        if (value == nullptr) {
            for (const Node* node : args) {
                check(node, nullptr);
            }
            return types_.error();
        }
        if (selector == "at:") {
            checkKey(args[0]);
            return record(SendKind::containerAt, isOptional(value) ? value : types_.optionalOf(value));
        }
        if (selector == "at:ifAbsent:") {
            checkKey(args[0]);
            const Type* fallback = checkInlinedBranch(args[1], "at:ifAbsent:", value);
            requireAssignable(args[1], fallback, value, "produce");
            return record(SendKind::containerAtIfAbsent, value);
        }
        if (selector == "includesKey:") {
            checkKey(args[0]);
            return record(SendKind::containerIncludes, types_.boolType());
        }
        if (selector == "removeKey:") {
            checkKey(args[0]);
            requireMutable(send->receiver, selector);
            return record(SendKind::containerRemove, types_.voidType());
        }
        if (selector == "keysDo:" || selector == "do:") {
            if (!checkInlinedEach(args[0], selector.c_str(), selector == "do:" ? value : type->key, 1)) {
                return types_.error();
            }
            return record(SendKind::inlineDo, types_.voidType());
        }
        if (selector == "keysAndValuesDo:") {
            auto literal = nodeCast<BlockLiteral>(args[0]);
            if (literal == nullptr || literal->params.size() != 2) {
                check(args[0], nullptr);
                error(args[0], "sema.inlinedNeedsBlock", "'keysAndValuesDo:' needs a block literal with two parameters, such as '{ [k, v] ... }'");
                return types_.error();
            }
            BlockInfo* info = newBlockInfo(literal, true);
            pushScope();
            const Type* bound[] = {type->key, value};
            for (size_t i = 0; i < 2; ++i) {
                auto param = static_cast<const BlockParam*>(literal->params[i]);
                LocalInfo* local = declareLocal(param->name, bound[i], param, param->range);
                local->readOnly = true;
                info->params.push_back(local);
                model_.localRefs[param->id] = local;
            }
            ++context_.loopDepth;
            checkStatements(literal->statements);
            --context_.loopDepth;
            popScope();
            remember(literal, types_.voidType());
            return record(SendKind::inlineDo, types_.voidType());
        }
    }
    if (type->kind == TypeKind::set && !isBitSet(type)) {
        if (selector == "add:" || selector == "remove:") {
            requireMutable(send->receiver, selector);
            const Type* element = elementOf(type, send, false);
            if (element == nullptr) {
                settleContainer(type, nullptr, check(args[0], nullptr), args[0]);
            }
            else {
                checkArgument(args[0], element);
            }
            return record(selector == "add:" ? SendKind::containerAdd : SendKind::containerRemove, types_.voidType());
        }
        if (selector == "includes:") {
            const Type* element = elementOf(type, send, true);
            if (element == nullptr) {
                return types_.error();
            }
            checkArgument(args[0], element);
            return record(SendKind::containerIncludes, types_.boolType());
        }
    }
    if (selector == "do:" && (type->kind == TypeKind::array || type->kind == TypeKind::fixedArray || type->kind == TypeKind::set)) {
        const Type* element = elementOf(type, send, true);
        if (element == nullptr || !checkInlinedEach(args[0], "do:", element, 1)) {
            return types_.error();
        }
        return record(SendKind::inlineDo, types_.voidType());
    }
    return nullptr;
}

}
