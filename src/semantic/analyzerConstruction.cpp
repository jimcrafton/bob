// Creating objects (design §4.3a): `x: C;`, constructor messages built from
// public fields, the automatic per-class `init`, and the generated `copy`.

#include "analyzerInternal.h"

#include <algorithm>

namespace bob::sema::detail {

// `msg init { ... }`: no parameters, no result, instance side. It isn't a
// message anyone can send; construction runs each class's own `init`,
// superclass first.
MessageInfo* Analyzer::declareInit(ClassInfo* info, const MessageDecl* decl, bool isStatic)
{
    if (info->isProtocol) {
        error(decl->selectorRange, "sema.invalidInit", "protocols are never constructed, so they can't declare 'init'");
        return nullptr;
    }
    if (isStatic || !decl->params.empty() || decl->returnType != nullptr) {
        error(decl->selectorRange, "sema.invalidInit",
              "'init' takes no arguments and returns nothing; it runs automatically after the fields are set. "
              "Pass values as fields ('" + info->name + " field: value'), or write a @[static] factory message");
        return nullptr;
    }
    if (info->initMessage != nullptr) {
        error(decl->selectorRange, "sema.redeclared", "'" + info->name + "' already declares 'init'");
        return nullptr;
    }
    if (decl->body == nullptr) {
        error(decl->selectorRange, "sema.missingBody", "'init' needs a body");
    }
    auto message = std::make_unique<MessageInfo>();
    message->selector = "init";
    message->decl = decl;
    message->owner = info;
    message->isInstance = true;
    message->isInit = true;
    message->overflow = overflowFrom(decl, unitOverflow_, {"overflow"});
    message->symbolName = "bob." + info->name + ".init";
    auto self = std::make_unique<LocalInfo>();
    self->name = "self";
    self->type = info->type;
    self->isParameter = true;
    self->readOnly = true;
    message->self = self.get();
    message->locals.push_back(self.get());
    model_.locals.push_back(std::move(self));
    message->returnType = types_.voidType();
    info->initMessage = message.get();
    MessageInfo* raw = message.get();
    model_.messages.push_back(std::move(message));
    return raw;
}

// Every concrete class gets `copy` (unless it declares one): an instance
// message with its own vtable slot, so `a copy` copies the dynamic class.
void Analyzer::addCopyMessage(ClassInfo* info)
{
    if (info->methods.count("copy") != 0) {
        return;
    }
    auto message = std::make_unique<MessageInfo>();
    message->selector = "copy";
    message->owner = info;
    message->isInstance = true;
    message->isCopy = true;
    message->returnType = info->type;
    message->overflow = unitOverflow_;
    message->symbolName = "bob." + info->name + ".copy";
    auto self = std::make_unique<LocalInfo>();
    self->name = "self";
    self->type = info->type;
    self->isParameter = true;
    self->readOnly = true;
    message->self = self.get();
    message->locals.push_back(self.get());
    model_.locals.push_back(std::move(self));
    MessageInfo* raw = message.get();
    info->methods["copy"] = raw;
    info->methodList.push_back(raw);
    checkState_[raw] = 2;
    model_.messages.push_back(std::move(message));
}

// A class can be default-constructed when every field has an initializer, a
// plain default (numbers, optionals, strings, containers ...), or is a class
// that can itself be default-constructed. A field that can't be defaulted is
// required: public ones must be passed to the constructor message, private
// ones are an error (nothing outside could ever set them).
bool Analyzer::resolveDefaultable(ClassInfo* info)
{
    if (info->defaultState == 2) {
        return info->defaultable;
    }
    if (info->defaultState == 1) {
        return false; // a cycle of non-optional fields: it can't bottom out
    }
    info->defaultState = 1;
    bool defaultable = !info->isProtocol;
    if (info->superclass != nullptr) {
        defaultable = resolveDefaultable(info->superclass) && defaultable;
    }
    for (FieldInfo& field : info->fields) {
        if (field.owner != info) {
            // Inherited fields are copies made before this analysis: take the superclass's result.
            const FieldInfo& inherited = info->superclass->fields[field.index];
            field.required = inherited.required;
            field.constructsDefault = inherited.constructsDefault;
            continue;
        }
        if (field.initializer != nullptr || !isClass(field.type)) {
            continue;
        }
        ClassInfo* fieldClass = field.type->classInfo;
        if (resolveDefaultable(fieldClass)) {
            field.constructsDefault = true;
            continue;
        }
        field.required = true;
        defaultable = false;
        if (!field.isPublic) {
            std::string why = fieldClass->isProtocol ? "'" + fieldClass->name + "' is a protocol"
                              : fieldClass == info  ? "'" + info->name + "' would contain itself"
                                                    : "'" + fieldClass->name + "' can't be built with default fields";
            error(field.decl->nameRange, "sema.uninitializedReference",
                  "private field '" + field.name + "' needs an initializer: " + why +
                      ", and only public fields can be passed when constructing; make it optional ('" + fieldClass->name + "?') or give it ':= …'");
        }
    }
    info->defaultState = 2;
    info->defaultable = defaultable;
    return defaultable;
}

// `Point x: 4 y: 7`: any of the class's public fields, in declaration order.
// Returns null when the selector isn't a constructor, so the caller reports
// an unknown message.
const Type* Analyzer::checkConstructor(const Node* send, ClassInfo* info, const std::string& selector, const std::vector<Node*>& arguments,
                                       SourceRange selectorRange)
{
    if (selector == "new") {
        error(selectorRange, "sema.unknownMessage",
              "there's no 'new'; declare 'x: " + info->name + ";' to construct one with default fields, or pass public fields: '" + info->name +
                  " field: value …'");
        return types_.error();
    }
    std::vector<std::string> parts = selectorParts(selector);
    if (parts.empty() || parts.size() != arguments.size() || parts.back().back() != ':') {
        return nullptr;
    }
    std::vector<const FieldInfo*> fields;
    for (const std::string& part : parts) {
        const FieldInfo* field = info->field(part.substr(0, part.size() - 1));
        if (field == nullptr || !field->isPublic) {
            return nullptr;
        }
        fields.push_back(field);
    }
    if (info->isProtocol) {
        error(selectorRange, "sema.newProtocol", "'" + info->name + "' is a protocol and can't be constructed");
        return types_.error();
    }
    for (size_t i = 1; i < fields.size(); ++i) {
        if (fields[i]->index <= fields[i - 1]->index) {
            for (const Node* argument : arguments) {
                check(argument, nullptr);
            }
            error(selectorRange, "sema.constructorOrder",
                  "pass fields in declaration order: '" + fields[i]->name + ":' is declared before '" + fields[i - 1]->name + ":'");
            return types_.error();
        }
    }
    if (info->fields.size() > 64) {
        unsupported(send, "constructing a class with more than 64 fields", "a later phase");
        return types_.error();
    }
    for (size_t i = 0; i < arguments.size(); ++i) {
        checkArgument(arguments[i], fields[i]->type);
    }
    std::string missing;
    for (const FieldInfo& field : info->fields) {
        if (field.required && std::find(fields.begin(), fields.end(), &field) == fields.end()) {
            missing += (missing.empty() ? "" : ", ") + field.name + ":";
        }
    }
    if (!missing.empty()) {
        error(selectorRange, "sema.missingConstructorArgument",
              "constructing a '" + info->name + "' needs " + missing + " (no default can be built for it)");
        return types_.error();
    }
    noteConstruct(info);
    addSend(send, SendKind::constructObject, selector, nullptr, info->type);
    model_.sends[send->id].classInfo = info;
    model_.sends[send->id].fields = fields;
    return info->type;
}

void Analyzer::checkDefaultObject(const VariableDecl* decl, const Type* type)
{
    ClassInfo* info = type->classInfo;
    if (info->isProtocol) {
        error(decl->nameRange, "sema.newProtocol",
              "'" + decl->name + "' is a '" + info->name + "', a protocol, which can't be constructed; give it a value or declare it '" +
                  info->name + "?'");
        return;
    }
    if (!resolveDefaultable(info)) {
        std::string missing;
        for (const FieldInfo& field : info->fields) {
            if (field.required) {
                missing += (missing.empty() ? "" : " ") + field.name + ": …";
            }
        }
        error(decl->nameRange, "sema.uninitializedReference",
              "'" + info->name + "' can't be built with default fields; pass them: '" + decl->name + " := " + info->name + " " + missing + ";'");
        return;
    }
    model_.defaultObjects[decl->id] = info;
    noteConstruct(info);
}

}

namespace bob::sema::detail {

void Analyzer::checkPlacementAttributes(const Node* statement)
{
    const Attribute* placement = nullptr;
    for (const Attribute* attribute : statement->attributes) {
        if (attribute->name != "stack" && attribute->name != "heap") {
            continue;
        }
        if (placement != nullptr) {
            error(attribute, "sema.invalidAttribute", "a declaration is either @[stack] or @[heap], not both");
            return;
        }
        placement = attribute;
    }
    if (placement == nullptr) {
        return;
    }
    const Node* value = nullptr;
    if (auto decl = nodeCast<VariableDecl>(statement)) {
        if (model_.defaultObjects.count(decl->id) != 0) {
            return;
        }
        value = decl->initializer;
    }
    else if (auto assignment = nodeCast<Assignment>(statement)) {
        value = assignment->value;
    }
    while (auto attributed = nodeCast<AttributedExpr>(value)) {
        value = attributed->expression;
    }
    const SendInfo* send = model_.sendOf(value);
    if (send != nullptr && send->kind == SendKind::constructObject) {
        return;
    }
    const Type* type = value != nullptr ? model_.typeOf(value) : model_.typeOf(statement);
    if (auto decl = nodeCast<VariableDecl>(statement); decl != nullptr && type == nullptr) {
        const LocalInfo* local = model_.localOf(decl);
        type = local != nullptr ? local->type : nullptr;
    }
    bool isValue = type != nullptr && !isReference(canonical(type));
    error(placement, "sema.invalidAttribute",
          "'@[" + placement->name + "]' applies to a declaration that creates an object ('x: C;' or 'x := C field: …')" +
              std::string(isValue ? "; records and other values already live in the variable itself" : ""));
}

}
