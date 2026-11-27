// Compile-time code (design §8): unit-level constants, @[compiletime]
// messages, expressions and blocks, `meta`, and the `compiler` object.
// Checking happens here; the compiler library evaluates the items
// (compiletime/evaluator.cpp) and the code generator embeds the results.

#include "analyzerInternal.h"
#include "layout.h"

namespace bob::sema::detail {

namespace {

// The expression a constant's initializer computes (`@[compiletime]` there is redundant).
const Node* stripCompileTime(const Node* node)
{
    auto attributed = nodeCast<AttributedExpr>(node);
    if (attributed != nullptr && attributed->attributes.size() == 1 && attributed->attributes[0]->name == "compiletime") {
        return attributed->expression;
    }
    return node;
}

bool embeddable(const Type* type)
{
    type = canonical(type);
    switch (type->kind) {
    case TypeKind::boolType:
    case TypeKind::integer:
    case TypeKind::real:
    case TypeKind::enumType:
    case TypeKind::subrange:
    case TypeKind::character:
    case TypeKind::string:
    case TypeKind::record:
        return true;
    case TypeKind::fixedArray:
    case TypeKind::array:
    case TypeKind::set:
    case TypeKind::optional:
        return !type->pending && embeddable(type->inner) && !isReference(type);
    case TypeKind::dictionary:
    case TypeKind::hashtable:
        return !type->pending && embeddable(type->inner) && embeddable(type->key);
    case TypeKind::unionType:
        for (const Type* member : type->members) {
            if (!embeddable(member)) {
                return false;
            }
        }
        return true;
    default:
        return false;
    }
}

}

// ---------------------------------------------------------------------------
// declarations

// `@[constant] NAME := value;` or `@[constant] name: T := value;`.
void Analyzer::declareConstant(const Node* declaration)
{
    std::string name;
    SourceRange nameRange;
    const Node* typeNode = nullptr;
    const Node* value = nullptr;
    if (auto assignment = nodeCast<Assignment>(declaration)) {
        auto target = nodeCast<Identifier>(assignment->target);
        if (target == nullptr) {
            error(assignment, "sema.invalidConstant", "a constant is declared '@[constant] NAME := value;'");
            return;
        }
        name = target->name;
        nameRange = target->range;
        value = assignment->value;
    }
    else {
        auto decl = static_cast<const VariableDecl*>(declaration);
        name = decl->name;
        nameRange = decl->nameRange;
        typeNode = decl->type;
        value = decl->initializer;
        if (value == nullptr) {
            error(decl->nameRange, "sema.invalidConstant", "constant '" + name + "' needs a value: '@[constant] " + name + ": T := value;'");
            return;
        }
    }
    if (isTypeName(name) || name == model_.program.name || name == "compiler" || model_.constantNamed(name) != nullptr) {
        error(nameRange, "sema.redeclared", "'" + name + "' is already declared");
        return;
    }
    for (const Attribute* attribute : declaration->attributes) {
        if (attribute->name != "constant" || attribute->value != nullptr) {
            unsupported(attribute, "the attribute '@[" + attribute->name + "]' on a constant", "a later phase");
        }
    }
    auto item = std::make_unique<CompileTimeItem>();
    item->kind = CompileTimeItem::Kind::constant;
    item->node = declaration;
    item->code = stripCompileTime(value);
    auto constant = std::make_unique<ConstantInfo>();
    constant->name = name;
    constant->decl = declaration;
    constant->nameRange = nameRange;
    constant->typeNode = typeNode;
    constant->value = value;
    constant->item = item.get();
    model_.compileTimeItems.push_back(std::move(item));
    model_.constants.push_back(std::move(constant));
}

void Analyzer::declareUnitBlock(const StatementGroup* group)
{
    auto item = std::make_unique<CompileTimeItem>();
    item->kind = CompileTimeItem::Kind::block;
    item->node = group;
    item->code = group;
    model_.compileTimeNodes[group->id] = item.get();
    model_.compileTimeItems.push_back(std::move(item));
}

MessageInfo* Analyzer::compileTimeContext(const std::string& name)
{
    auto message = std::make_unique<MessageInfo>();
    message->selector = name;
    message->symbolName = "bob." + model_.program.name + ".compiletime." + std::to_string(model_.compileTimeItems.size()) + "." + name;
    message->overflow = unitOverflow_;
    message->returnType = types_.voidType();
    message->compileTimeOnly = true;
    MessageInfo* raw = message.get();
    model_.messages.push_back(std::move(message));
    return raw;
}

// ---------------------------------------------------------------------------
// checking

const Type* Analyzer::checkCompileTimeItem(CompileTimeItem* item, const Type* expected)
{
    if (item->context == nullptr) {
        item->context = compileTimeContext(item->kind == CompileTimeItem::Kind::block ? "block" : "expression");
    }
    FunctionContext savedContext = context_;
    auto savedScopes = std::move(scopes_);
    auto savedOverflow = std::move(overflow_);
    scopes_.clear();
    context_ = FunctionContext{item->context, nullptr, &item->context->locals, false, 0};
    context_.compileTime = true;
    overflow_ = {unitOverflow_};
    pushScope();
    const Type* type = types_.voidType();
    if (item->kind == CompileTimeItem::Kind::block) {
        checkStatements(static_cast<const StatementGroup*>(item->code)->statements);
    }
    else {
        type = check(item->code, expected);
    }
    popScope();
    context_ = savedContext;
    scopes_ = std::move(savedScopes);
    overflow_ = std::move(savedOverflow);
    item->type = type;
    if (context_.message != nullptr) {
        context_.message->itemsUsed.push_back(item);
    }
    return type;
}

void Analyzer::checkConstant(ConstantInfo* constant)
{
    if (constant->state == 2) {
        return;
    }
    if (constant->state == 1) {
        error(constant->nameRange, "sema.constantCycle", "constant '" + constant->name + "' is used while its own value is being worked out");
        constant->type = types_.error();
        return;
    }
    constant->state = 1;
    constant->item->context = compileTimeContext(constant->name);
    const Type* declared = constant->typeNode != nullptr ? resolveType(constant->typeNode) : nullptr;
    FunctionContext saved = context_;
    context_.message = nullptr;
    const Type* type = checkCompileTimeItem(constant->item, declared);
    context_ = saved;
    if (declared != nullptr) {
        // `@[constant] name: T := value;`: the value converts to T as an initializer would.
        requireAssignable(constant->value, type, declared, "initialize");
        type = declared;
        constant->item->type = declared;
    }
    if (type != types_.error() && (canonical(type)->pending || type->kind == TypeKind::nilType || !isValueType(type))) {
        error(constant->value, "sema.cannotInfer", "can't infer the type of constant '" + constant->name + "'; write it: '@[constant] " +
                                                       constant->name + ": T := value;'");
        type = types_.error();
    }
    if (!requireEmbeddable(constant->value, type, "a constant")) {
        type = types_.error();
    }
    if (constant->type == nullptr) {
        constant->type = type;
    }
    constant->state = 2;
}

bool Analyzer::requireEmbeddable(const Node* node, const Type* type, const char* what)
{
    if (type == types_.error() || embeddable(type)) {
        return true;
    }
    error(node, "sema.notEmbeddable",
          std::string("'") + typeName(type) + "' can't be " + what +
              ": only values (numbers, strings, enums, records, and containers of those) are computed at compile time and embedded "
              "in the program; objects and blocks exist only while code runs");
    return false;
}

const Type* Analyzer::checkCompileTimeExpression(const AttributedExpr* expression, const Type* expected)
{
    for (const Attribute* attribute : expression->attributes) {
        if (attribute->name != "compiletime") {
            unsupported(attribute, "combining '@[" + attribute->name + "]' with '@[compiletime]'", "a later phase");
        }
    }
    if (context_.compileTime) {
        return check(expression->expression, expected); // already compile-time code
    }
    auto item = std::make_unique<CompileTimeItem>();
    item->kind = CompileTimeItem::Kind::expression;
    item->node = expression;
    item->code = expression->expression;
    CompileTimeItem* raw = item.get();
    model_.compileTimeItems.push_back(std::move(item));
    model_.compileTimeNodes[expression->id] = raw;
    const Type* type = checkCompileTimeItem(raw, expected);
    return requireEmbeddable(expression, type, "the result of compile-time code") ? type : types_.error();
}

void Analyzer::checkCompileTimeBlock(const StatementGroup* group)
{
    if (context_.compileTime) {
        pushScope();
        checkStatements(group->statements);
        popScope();
        return;
    }
    auto item = std::make_unique<CompileTimeItem>();
    item->kind = CompileTimeItem::Kind::block;
    item->node = group;
    item->code = group;
    CompileTimeItem* raw = item.get();
    model_.compileTimeItems.push_back(std::move(item));
    model_.compileTimeNodes[group->id] = raw;
    checkCompileTimeItem(raw, nullptr);
}

void Analyzer::noteCall(const Node* site, const MessageInfo* target)
{
    if (target == nullptr) {
        return;
    }
    if (target->compileTimeOnly && !context_.compileTime) {
        error(site, "sema.compileTimeOnly",
              "'" + target->selector + "' is @[compiletime]: call it from compile-time code, such as '@[compiletime] (…)' or a constant");
    }
    if (context_.message != nullptr) {
        context_.message->calls.push_back(target);
    }
}

void Analyzer::noteConstruct(const ClassInfo* info)
{
    if (context_.message != nullptr && info != nullptr) {
        context_.message->constructs.push_back(info);
    }
}

// ---------------------------------------------------------------------------
// `meta` and `compiler`

const ConstValue* Analyzer::makeConst(ConstValue value)
{
    model_.constValues.push_back(std::make_unique<ConstValue>(std::move(value)));
    return model_.constValues.back().get();
}

const Type* Analyzer::checkMeta(const UnarySend* send, const Type* receiver)
{
    const Type* subject = receiver;
    std::string name;
    if (receiver->kind == TypeKind::meta) {
        if (receiver->instance == nullptr) {
            error(send->selectorRange, "sema.invalidMeta", "the program itself has no 'meta' information");
            return types_.error();
        }
        subject = receiver->instance;
        name = typeName(subject);
    }
    else if (receiver->kind == TypeKind::metaInfo || receiver->kind == TypeKind::compilerObject || !isValueType(receiver)) {
        error(send->selectorRange, "sema.invalidMeta", "'meta' needs a value or a type, not '" + typeName(receiver) + "'");
        return types_.error();
    }
    else if (auto identifier = nodeCast<Identifier>(send->receiver)) {
        name = identifier->name;
    }
    else if (auto access = nodeCast<FieldAccess>(send->receiver)) {
        name = access->field;
    }
    subject = canonical(subject);
    const Type* result = types_.metaInfoOf(subject);
    addSend(send, SendKind::metaInfo, name, subject, result);
    return result;
}

const Type* Analyzer::checkMetaQuery(const UnarySend* send, const Type* metaType)
{
    const Type* subject = canonical(metaType->inner);
    const SendInfo* source = model_.sendOf(send->receiver);
    std::string name = source != nullptr ? source->op : "";
    const std::string& selector = send->selector;
    auto constant = [&](ConstValue value) {
        const Type* type = value.type;
        addSend(send, SendKind::constantValue, selector, subject, type);
        model_.sends[send->id].value = makeConst(std::move(value));
        return type;
    };
    auto integer = [&](uint64_t bits) {
        ConstValue value;
        value.type = types_.int64();
        value.bits = bits;
        return constant(value);
    };
    auto text = [&](const std::string& s) {
        ConstValue value;
        value.type = types_.string();
        value.text = s;
        return constant(value);
    };
    if (selector == "size") {
        return integer(isClass(subject) ? objectSize(subject->classInfo) : sizeOf(subject));
    }
    if (selector == "alignment") {
        return integer(isClass(subject) ? 8 : alignmentOf(subject));
    }
    if (selector == "typeName") {
        return text(typeName(subject));
    }
    if (selector == "name") {
        if (name.empty()) {
            error(send->selectorRange, "sema.invalidMeta", "this expression has no name; 'meta name' works on variables, fields and types");
            return types_.error();
        }
        return text(name);
    }
    if (selector == "type") {
        addSend(send, SendKind::metaInfo, typeName(subject), subject, metaType);
        return metaType;
    }
    if (selector == "elementType" && subject->inner != nullptr &&
        (subject->kind == TypeKind::array || subject->kind == TypeKind::fixedArray || subject->kind == TypeKind::set ||
         subject->kind == TypeKind::dictionary || subject->kind == TypeKind::hashtable || isOptional(subject))) {
        const Type* element = canonical(subject->inner);
        addSend(send, SendKind::metaInfo, typeName(element), element, types_.metaInfoOf(element));
        return types_.metaInfoOf(element);
    }
    if (selector == "count") {
        if (isEnum(subject)) {
            return integer(subject->enumInfo->members.size());
        }
        if (subject->kind == TypeKind::fixedArray) {
            return integer(subject->count);
        }
        if (isSubrange(subject)) {
            return integer(static_cast<uint64_t>(subject->high - subject->low) + 1);
        }
        if (subject->kind == TypeKind::record) {
            return integer(subject->record->fields.size());
        }
    }
    if (selector == "members") {
        if (isEnum(subject)) {
            ConstValue value;
            value.type = types_.arrayOf(subject);
            for (const EnumMemberInfo& member : subject->enumInfo->members) {
                ConstValue element;
                element.type = subject;
                element.bits = member.value;
                value.elements.push_back(element);
            }
            return constant(value);
        }
        if (isUnion(subject)) {
            ConstValue value;
            value.type = types_.arrayOf(types_.string());
            for (const Type* member : subject->members) {
                ConstValue element;
                element.type = types_.string();
                element.text = typeName(member);
                value.elements.push_back(element);
            }
            return constant(value);
        }
    }
    if (selector == "fields" && (subject->kind == TypeKind::record || (isClass(subject) && !subject->classInfo->isProtocol))) {
        if (!context_.compileTime) {
            error(send->selectorRange, "sema.compileTimeOnly",
                  "'meta fields' gives FieldMeta objects, which exist only in compile-time code; use it inside '@[compiletime]'");
            return types_.error();
        }
        ClassInfo* fieldMeta = fieldMetaClass();
        std::vector<const FieldInfo*> fields;
        const std::vector<FieldInfo>& all = subject->kind == TypeKind::record ? subject->record->fields : subject->classInfo->fields;
        for (const FieldInfo& field : all) {
            if (field.owner == nullptr || field.isPublic || canAccessField(&field)) {
                fields.push_back(&field);
            }
        }
        const Type* result = types_.arrayOf(fieldMeta->type);
        addSend(send, SendKind::metaFields, selector, subject, result);
        model_.sends[send->id].fields = fields;
        model_.sends[send->id].classInfo = fieldMeta;
        noteConstruct(fieldMeta);
        return result;
    }
    error(send->selectorRange, "sema.unknownMessage",
          "'meta' of '" + typeName(subject) + "' has no '" + selector +
              "'; it knows size, alignment, typeName, name, type, count, members, elementType and fields");
    return types_.error();
}

// `FieldMeta { name; typeName; offset; size }`: the elements of `T meta fields`.
ClassInfo* Analyzer::fieldMetaClass()
{
    if (model_.fieldMetaClass != nullptr) {
        return model_.fieldMetaClass;
    }
    auto info = std::make_unique<ClassInfo>();
    info->name = "FieldMeta";
    info->type = types_.addClass(info.get());
    const std::pair<const char*, const Type*> fields[] = {
        {"name", types_.string()}, {"typeName", types_.string()}, {"offset", types_.int64()}, {"size", types_.int64()}};
    for (const auto& [fieldName, type] : fields) {
        FieldInfo field;
        field.name = fieldName;
        field.type = type;
        field.index = static_cast<uint32_t>(info->fields.size());
        field.owner = info.get();
        field.isPublic = true;
        info->fields.push_back(field);
    }
    info->resolved = true;
    ClassInfo* raw = info.get();
    model_.classes.push_back(std::move(info));
    completeClass(raw);
    resolveDefaultable(raw);
    model_.fieldMetaClass = raw;
    return raw;
}

const Type* Analyzer::checkCompilerSend(const KeywordSend* send)
{
    const std::string& selector = send->selector;
    if (selector == "error:" || selector == "warning:" || selector == "note:") {
        checkArgument(send->arguments[0], types_.string());
        addSend(send, SendKind::compilerDiagnostic, selector, nullptr, types_.voidType());
        return types_.voidType();
    }
    for (const Node* argument : send->arguments) {
        check(argument, nullptr);
    }
    error(send->partRanges.empty() ? send->range : send->partRanges.front(), "sema.unknownMessage",
          "'compiler' understands 'error:', 'warning:' and 'note:'");
    return types_.error();
}

}
