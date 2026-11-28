// Reflection through the class object (design §9): `obj class`, `Button class`
// and the messages a `Class` understands at the identity level.

#include "analyzerInternal.h"

namespace bob::sema::detail {

const Type* Analyzer::checkReflectionUnary(const UnarySend* send, const Type* receiver, const Type* expected)
{
    const std::string& selector = send->selector;
    const Type* classObject = types_.classObject();
    if (isClass(receiver) && (selector == "class" || selector == "isOnStack")) {
        bool isClassSend = selector == "class";
        const Type* result = isClassSend ? classObject : types_.boolType();
        addSend(send, isClassSend ? SendKind::classOf : SendKind::objectIsOnStack, selector, receiver, result);
        return result;
    }
    if (receiver->kind == TypeKind::meta && isClass(receiver->instance) && selector == "class") {
        const ClassInfo* info = receiver->instance->classInfo;
        if (info->isProtocol) {
            error(send->selectorRange, "sema.unknownMessage",
                  "'" + info->name + "' is a protocol and has no class object; ask an object for its class with 'obj class'");
            return types_.error();
        }
        addSend(send, SendKind::namedClass, selector, nullptr, classObject);
        model_.sends[send->id].classInfo = info;
        return classObject;
    }
    if (canonical(receiver)->kind != TypeKind::classObject) {
        return nullptr;
    }
    if (selector == "name") {
        addSend(send, SendKind::className, selector, classObject, types_.string());
        return types_.string();
    }
    if (selector == "superclass") {
        const Type* result = types_.optionalOf(classObject);
        addSend(send, SendKind::classSuperclass, selector, classObject, result);
        return result;
    }
    if (selector == "fields" || selector == "messages") {
        bool fields = selector == "fields";
        ClassInfo* metaClass = fields ? fieldMetaClass() : messageMetaClass();
        const Type* result = types_.arrayOf(metaClass->type);
        addSend(send, fields ? SendKind::classFields : SendKind::classMessages, selector, classObject, result);
        model_.sends[send->id].classInfo = metaClass;
        noteConstruct(metaClass);
        return result;
    }
    if (selector == "protocols") {
        const Type* result = types_.arrayOf(types_.string());
        addSend(send, SendKind::classProtocols, selector, classObject, result);
        return result;
    }
    if (selector == "qualifiedName") {
        addSend(send, SendKind::classQualifiedName, selector, classObject, types_.string());
        return types_.string();
    }
    if (selector == "new") {
        return checkClassNew(send, expected);
    }
    error(send->selectorRange, "sema.unknownMessage", "'Class' doesn't understand '" + selector + "'");
    return types_.error();
}

const Type* Analyzer::checkClassObjectKeyword(const KeywordSend* send, const Type* expected)
{
    const Type* classObject = types_.classObject();
    const std::string& selector = send->selector;
    SourceRange selectorRange = send->partRanges.empty() ? send->range : send->partRanges.front();
    if (selector == "get:of:") {
        return checkGetField(send, expected);
    }
    if (selector == "set:of:to:") {
        return checkSetField(send);
    }
    if (selector.rfind("perform:on:", 0) == 0) {
        return checkPerform(send, expected);
    }
    if (selector == "isKindOf:" || selector == "respondsTo:") {
        bool kindOf = selector == "isKindOf:";
        checkArgument(send->arguments[0], kindOf ? classObject : types_.string());
        addSend(send, kindOf ? SendKind::classIsKindOf : SendKind::classRespondsTo, selector, classObject, types_.boolType());
        return types_.boolType();
    }
    for (const Node* argument : send->arguments) {
        check(argument, nullptr);
    }
    error(selectorRange, "sema.unknownMessage", "'Class' doesn't understand '" + selector + "'");
    return types_.error();
}

// `c get: "width" of: obj`: the result type comes from context and is checked
// against the field's stored type at runtime. With a literal name on a class
// named statically (`Rect class get: "w" of: r`) the check happens here instead.
namespace {

bool hasContextType(const Type* expected)
{
    return expected != nullptr && expected->kind != TypeKind::error && canonical(expected)->kind != TypeKind::voidType;
}

size_t colonCount(const std::string& selector)
{
    size_t count = 0;
    for (char c : selector) {
        if (c == ':') {
            ++count;
        }
    }
    return count;
}

}

const ClassInfo* Analyzer::staticClassOf(const Node* receiver) const
{
    const SendInfo* send = model_.sendOf(receiver);
    return send != nullptr && send->kind == SendKind::namedClass ? send->classInfo : nullptr;
}

bool Analyzer::noteInvokeUse(const Node* site, const std::string& what)
{
    if (context_.compileTime) {
        error(site, "sema.reflectorRequired", what + " acts through reflection, which compile-time code can't do");
        return false;
    }
    if (firstInvokeUse_ == nullptr) {
        firstInvokeUse_ = site;
    }
    return true;
}

// A program that acts through reflection, or imports a module that does, must
// say so on main (design §9: the Reflector is a build-time switch).
void Analyzer::checkReflector(const Unit* unit)
{
    const ModuleInfo* needing = nullptr;
    for (const auto& module : model_.modules) {
        if (module->needsReflector && needing == nullptr) {
            needing = module.get();
        }
    }
    model_.needsReflector = firstInvokeUse_ != nullptr || needing != nullptr;
    const MessageInfo* main = model_.program.main;
    if (unit->unitKind != UnitKind::program || !model_.needsReflector || main == nullptr || mainHasReflector_) {
        return;
    }
    std::string fix = "; declare a Reflector on main: 'msg main: args:array reflector: r:Reflector [int32]'";
    if (firstInvokeUse_ != nullptr) {
        error(firstInvokeUse_, "sema.reflectorRequired", "this acts through reflection" + fix);
    }
    else {
        error(main->decl->selectorRange, "sema.reflectorRequired", "the imported module '" + needing->name + "' acts through reflection" + fix);
    }
}

// The field a literal name selects on a class named statically, checked for
// what a reflective `get:of:` (introspect) or `set:of:to:` (invoke) needs.
static const FieldInfo* reflectedField(const ClassInfo* info, const std::string& name)
{
    const FieldInfo* field = info->field(name);
    return field != nullptr && field->isPublic ? field : nullptr;
}

// `c get: "width" of: obj`: the result type comes from context and is checked
// against the field's stored type at runtime. With a literal name on a class
// named statically (`Rect class get: "w" of: r`) the check happens here instead.
const Type* Analyzer::checkGetField(const KeywordSend* send, const Type* expected)
{
    SourceRange selectorRange = send->partRanges.empty() ? send->range : send->partRanges.front();
    checkArgument(send->arguments[0], types_.string());
    const Type* object = check(send->arguments[1], nullptr);
    if (object == types_.error()) {
        return types_.error();
    }
    if (!isClass(object)) {
        error(send->arguments[1], "sema.typeMismatch", "'get:of:' reads a field of an object, not a '" + typeName(object) + "'");
        return types_.error();
    }
    if (!hasContextType(expected)) {
        error(selectorRange, "sema.reflectNeedsType",
              "'get:of:' takes its result type from context; declare it, as in 'w: int32 := c get: \"w\" of: obj'");
        return types_.error();
    }
    const ClassInfo* info = staticClassOf(send->receiver);
    auto name = nodeCast<StringLiteral>(send->arguments[0]);
    if (info != nullptr && name != nullptr) {
        const FieldInfo* field = reflectedField(info, name->value);
        if (field == nullptr) {
            error(send->arguments[0], "sema.unknownField", "'" + info->name + "' has no public field '" + name->value + "'");
            return types_.error();
        }
        if (field->owner->reflectLevel < ReflectLevel::introspect) {
            error(send->arguments[0], "sema.reflectLevel",
                  "'" + field->owner->name + "' is emitted at @[reflect: " + reflectLevelName(field->owner->reflectLevel) +
                      "], which has no field metadata; 'get:of:' needs 'introspect'");
            return types_.error();
        }
        if (field->ownership != Ownership::strong) {
            error(send->arguments[0], "sema.typeMismatch", "'" + name->value + "' is weak or unowned; reflection reads only strong fields");
            return types_.error();
        }
        if (canonical(field->type) != canonical(expected)) {
            error(selectorRange, "sema.typeMismatch",
                  "'" + info->name + " " + name->value + "' is a '" + typeName(field->type) + "', not a '" + typeName(expected) + "'");
            return types_.error();
        }
    }
    addSend(send, SendKind::classGetField, send->selector, types_.classObject(), expected);
    return expected;
}

// `c set: "width" of: obj to: value`: the value's own type must be the field's.
const Type* Analyzer::checkSetField(const KeywordSend* send)
{
    noteInvokeUse(send, "'set:of:to:'");
    checkArgument(send->arguments[0], types_.string());
    const Type* object = check(send->arguments[1], nullptr);
    const ClassInfo* info = staticClassOf(send->receiver);
    auto name = nodeCast<StringLiteral>(send->arguments[0]);
    const FieldInfo* field = info != nullptr && name != nullptr ? reflectedField(info, name->value) : nullptr;
    if (info != nullptr && name != nullptr) {
        if (field == nullptr) {
            error(send->arguments[0], "sema.unknownField", "'" + info->name + "' has no public field '" + name->value + "'");
        }
        else if (field->owner->reflectLevel < ReflectLevel::invoke) {
            error(send->arguments[0], "sema.reflectLevel",
                  "'" + field->owner->name + "' is emitted at @[reflect: " + reflectLevelName(field->owner->reflectLevel) +
                      "]; writing a field by reflection needs 'invoke'");
            field = nullptr;
        }
        else if (field->ownership != Ownership::strong) {
            error(send->arguments[0], "sema.typeMismatch", "'" + name->value + "' is weak or unowned; reflection writes only strong fields");
            field = nullptr;
        }
    }
    const Type* value = nullptr;
    if (field != nullptr) {
        checkArgument(send->arguments[2], field->type);
        value = field->type;
    }
    else {
        value = check(send->arguments[2], nullptr);
    }
    if (object == types_.error() || value == types_.error()) {
        return types_.error();
    }
    if (!isClass(object)) {
        error(send->arguments[1], "sema.typeMismatch", "'set:of:to:' writes a field of an object, not a '" + typeName(object) + "'");
        return types_.error();
    }
    if (canonical(value)->kind == TypeKind::voidType || value->kind == TypeKind::nilType) {
        error(send->arguments[2], "sema.reflectNeedsType", "'set:of:to:' takes the field's type from the value; give it a type");
        return types_.error();
    }
    addSend(send, SendKind::classSetField, send->selector, field != nullptr ? field->type : value, types_.voidType());
    return types_.voidType();
}

// `c perform: "scale:by:" on: obj with: 2 with: 3`: argument types from the
// argument expressions, the result type from context (none: the message must
// return nothing). With a literal selector on a class named statically the
// message is looked up here and its own signature is used.
const Type* Analyzer::checkPerform(const KeywordSend* send, const Type* expected)
{
    SourceRange selectorRange = send->partRanges.empty() ? send->range : send->partRanges.front();
    size_t argumentCount = send->arguments.size() - 2;
    std::string withs;
    for (size_t i = 0; i < argumentCount; ++i) {
        withs += "with:";
    }
    if (send->selector != "perform:on:" + withs || argumentCount > 3) {
        for (const Node* argument : send->arguments) {
            check(argument, nullptr);
        }
        error(selectorRange, "sema.unknownMessage", "'Class' performs with 'perform:on:', then up to three 'with:' arguments");
        return types_.error();
    }
    noteInvokeUse(send, "'perform:'");
    checkArgument(send->arguments[0], types_.string());
    const Type* object = check(send->arguments[1], nullptr);
    const ClassInfo* info = staticClassOf(send->receiver);
    auto selector = nodeCast<StringLiteral>(send->arguments[0]);
    const MessageInfo* target = nullptr;
    bool failed = object == types_.error();
    if (selector != nullptr && colonCount(selector->value) != argumentCount) {
        error(send->arguments[0], "sema.blockArity",
              "'" + selector->value + "' takes " + std::to_string(colonCount(selector->value)) + " argument(s), but " +
                  std::to_string(argumentCount) + " are passed");
        failed = true;
    }
    else if (info != nullptr && selector != nullptr) {
        target = info->lookup(selector->value);
        if (target == nullptr || target->isInit || !target->isInstance) {
            error(send->arguments[0], "sema.unknownMessage", "'" + info->name + "' doesn't understand '" + selector->value + "'");
            target = nullptr;
            failed = true;
        }
        else if (info->reflectLevel < ReflectLevel::invoke) {
            error(send->arguments[0], "sema.reflectLevel",
                  "'" + info->name + "' is emitted at @[reflect: " + reflectLevelName(info->reflectLevel) + "]; 'perform:' needs 'invoke'");
            failed = true;
        }
    }
    std::vector<const Type*> argumentTypes;
    for (size_t i = 0; i < argumentCount; ++i) {
        const Node* argument = send->arguments[i + 2];
        const Type* type = nullptr;
        if (target != nullptr) {
            checkArgument(argument, target->params[i]->type);
            type = target->params[i]->type;
        }
        else {
            type = check(argument, nullptr);
        }
        if (type == types_.error()) {
            failed = true;
        }
        else if (canonical(type)->kind == TypeKind::voidType || type->kind == TypeKind::nilType) {
            error(argument, "sema.reflectNeedsType", "a 'perform:' argument needs a type of its own");
            failed = true;
        }
        argumentTypes.push_back(type);
    }
    if (!failed && !isClass(object)) {
        error(send->arguments[1], "sema.typeMismatch", "'perform:' sends to an object, not a '" + typeName(object) + "'");
        failed = true;
    }
    if (failed) {
        return types_.error();
    }
    const Type* result = hasContextType(expected) ? expected : types_.voidType();
    if (target != nullptr) {
        result = target->returnType != nullptr ? target->returnType : types_.voidType();
    }
    addSend(send, SendKind::classPerform, send->selector, types_.classObject(), result);
    model_.sends[send->id].argumentTypes = argumentTypes;
    return result;
}

// `b: Button := c new`: a default-constructed object of the class `c` stands
// for, which must be the class (or protocol) the context expects.
const Type* Analyzer::checkClassNew(const UnarySend* send, const Type* expected)
{
    noteInvokeUse(send, "'new'");
    const Type* wanted = expected != nullptr ? canonical(expected) : nullptr;
    if (wanted != nullptr && isOptional(wanted)) {
        wanted = canonical(wanted->inner);
    }
    if (wanted == nullptr || !isClass(wanted)) {
        error(send->selectorRange, "sema.reflectNeedsType",
              "'new' takes its type from context, a class or protocol; declare it, as in 'b: Button := c new'");
        return types_.error();
    }
    if (const ClassInfo* info = staticClassOf(send->receiver)) {
        if (!info->isKindOf(wanted->classInfo)) {
            error(send->selectorRange, "sema.typeMismatch", "a new '" + info->name + "' isn't a '" + wanted->classInfo->name + "'");
            return types_.error();
        }
        if (info->reflectLevel < ReflectLevel::invoke || !info->defaultable) {
            error(send->selectorRange, "sema.reflectLevel",
                  "'" + info->name + "' can't be created by reflection: it needs @[reflect: invoke] and a default for every field");
            return types_.error();
        }
    }
    addSend(send, SendKind::classNew, send->selector, types_.classObject(), wanted);
    model_.sends[send->id].classInfo = wanted->classInfo;
    return wanted;
}

// `Class named: "unit.Button"` (or a bare name that only one class has).
const Type* Analyzer::checkClassNamed(const KeywordSend* send)
{
    if (send->selector != "named:") {
        for (const Node* argument : send->arguments) {
            check(argument, nullptr);
        }
        error(send->partRanges.empty() ? send->range : send->partRanges.front(), "sema.unknownMessage", "'Class' understands 'named:'");
        return types_.error();
    }
    noteInvokeUse(send, "'Class named:'");
    checkArgument(send->arguments[0], types_.string());
    const Type* result = types_.optionalOf(types_.classObject());
    addSend(send, SendKind::classNamed, send->selector, nullptr, result);
    return result;
}

const Type* Analyzer::checkClassObjectBinary(const BinarySend* send)
{
    const Type* classObject = types_.classObject();
    if (send->selector != "=" && send->selector != "<>") {
        check(send->argument, nullptr);
        error(send->selectorRange, "sema.unknownMessage", "classes compare only with '=' and '<>'");
        return types_.error();
    }
    checkArgument(send->argument, classObject);
    addSend(send, SendKind::classCompare, send->selector, classObject, types_.boolType());
    return types_.boolType();
}

}
