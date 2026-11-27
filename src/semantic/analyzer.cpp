#include "analyzer.h"

#include <algorithm>
#include <string_view>

namespace bob::sema {

namespace {

using namespace bob::ast;

std::string capitalize(const std::string& name)
{
    std::string out = name;
    if (out.rfind("uint", 0) == 0) {
        return "UInt" + out.substr(4);
    }
    if (!out.empty()) {
        out[0] = static_cast<char>(out[0] - 'a' + 'A');
    }
    return out;
}

bool isArithmeticOperator(std::string_view op)
{
    return op == "+" || op == "-" || op == "*" || op == "/" || op == "%";
}

bool isBitwiseOperator(std::string_view op)
{
    return op == "&" || op == "|" || op == "^";
}

bool isComparisonOperator(std::string_view op)
{
    return op == "=" || op == "<>" || op == "<" || op == "<=" || op == ">" || op == ">=";
}

bool hasAttribute(const Node* node, std::string_view name)
{
    for (const Attribute* attribute : node->attributes) {
        if (attribute->name == name) {
            return true;
        }
    }
    return false;
}

// Number of arguments for `value`, `value:`, `value:value:` ...; -1 otherwise.
int valueSelectorArity(const std::string& selector)
{
    if (selector == "value") {
        return 0;
    }
    if (selector.empty() || selector.size() % 6 != 0) {
        return -1;
    }
    for (size_t i = 0; i < selector.size(); i += 6) {
        if (selector.compare(i, 6, "value:") != 0) {
            return -1;
        }
    }
    return static_cast<int>(selector.size() / 6);
}

// Where the statements being checked run: the message body or a block function.
// Inlined blocks keep the context of the code around them.
struct FunctionContext {
    MessageInfo* message = nullptr;
    BlockInfo* block = nullptr;
    std::vector<LocalInfo*>* locals = nullptr;
    bool allowReturn = true;
    int loopDepth = 0;
};

class Analyzer {
public:
    Analyzer(SemanticModel& model, DiagnosticEngine& diagnostics) : model_(model), types_(model.types), diagnostics_(diagnostics) {}

    void run(const Unit* unit);

private:
    void error(SourceRange range, const char* code, const std::string& message) { diagnostics_.error(range, code, message); }
    void error(const Node* node, const char* code, const std::string& message) { error(node->range, code, message); }
    void unsupported(const Node* node, const std::string& what, const char* when)
    {
        error(node, "sema.unsupported", what + " isn't supported yet (planned for " + when + ")");
    }

    void collect(const Unit* unit);
    void declareRecord(const RecordDecl* decl);
    void resolveRecord(RecordInfo* record);
    void declareProgramMessages(const ExtendsDecl* decl);
    void declareMessage(const MessageDecl* decl);
    const Type* resolveType(const Node* node);
    const Type* resolveBlockType(const TypeMessage* message);
    bool requireValueType(const Node* node, const Type* type);
    bool rejectEscapingBlock(const Node* node, const Type* type, const char* where);
    OverflowMode overflowFrom(const Node* node, OverflowMode inherited, std::initializer_list<std::string_view> allowed);

    void ensureChecked(MessageInfo* message);
    void checkMessage(MessageInfo* message);
    void finalizeInferredBlocks(MessageInfo* message);
    void checkStatements(const std::vector<Node*>& statements);
    void checkGroup(const Node* group);
    void checkStatement(const Node* statement);
    void checkAssignment(const Assignment* assignment);
    void checkReturn(const ReturnStmt* statement);
    void checkFor(const ForStmt* statement);
    void checkCase(const CaseStmt* statement);
    void checkCondition(const Node* condition, const char* statement);

    const Type* check(const Node* expression, const Type* expected);
    const Type* checkIntegerLiteral(const IntegerLiteral* literal, const Type* expected);
    const Type* checkRealLiteral(const RealLiteral* literal, const Type* expected);
    const Type* checkIdentifier(const Identifier* identifier);
    const Type* checkFieldAccess(const FieldAccess* access);
    const Type* checkUnary(const UnarySend* send, const Type* expected);
    const Type* checkBinary(const BinarySend* send, const Type* expected);
    const Type* checkKeyword(const KeywordSend* send, const Type* expected);
    const Type* checkOperandPair(const Node* lhs, const Node* rhs, const Type* expected, const char* what, const Node* site);
    void checkArgument(const Node* argument, const Type* parameterType);
    bool requireAssignable(const Node* node, const Type* from, const Type* to, const char* verb);
    bool isUntypedLiteral(const Node* node) const;
    const Type* remember(const Node* node, const Type* type);
    void addSend(const Node* node, SendKind kind, std::string op, const Type* operandType, const Type* resultType);

    const Type* checkBlockLiteral(const BlockLiteral* block, const Type* expected);
    const Type* checkBlockValue(const std::vector<Node*>& statements, const Type* expected, bool hasExpectation);
    const Type* checkBlockInvoke(const Node* send, const Type* blockType, const std::vector<Node*>& arguments, const Type* expected);
    bool unify(const Type* pending, const Type* concrete, const Node* site);
    BlockInfo* newBlockInfo(const BlockLiteral* literal, bool inlined);
    const Type* checkInlinedBranch(const Node* argument, const char* selector, const Type* expected);
    const Type* checkInlinedIf(const KeywordSend* send, const Type* expected);
    const Type* checkInlinedToDo(const KeywordSend* send);
    const Type* checkInlinedTimesRepeat(const KeywordSend* send, const Type* receiver);

    void pushScope() { scopes_.emplace_back(); }
    void popScope() { scopes_.pop_back(); }
    LocalInfo* declareLocal(const std::string& name, const Type* type, const Node* decl, SourceRange nameRange);
    LocalInfo* lookupLocal(const std::string& name);
    const LocalInfo* rootLocal(const Node* node) const;
    bool isTypeName(const std::string& name) const;

    bool alwaysReturns(const std::vector<Node*>& statements) const;
    bool alwaysReturns(const Node* statement) const;
    bool breaksOut(const Node* node, int depth) const;

    OverflowMode overflow() const { return overflow_.empty() ? OverflowMode::trap : overflow_.back(); }

    SemanticModel& model_;
    TypeTable& types_;
    DiagnosticEngine& diagnostics_;
    const Unit* unit_ = nullptr;
    FunctionContext context_;
    std::vector<std::unordered_map<std::string, LocalInfo*>> scopes_;
    std::vector<OverflowMode> overflow_;
    OverflowMode unitOverflow_ = OverflowMode::trap;
    std::unordered_map<const MessageInfo*, int> checkState_;
    int blockCounter_ = 0;
};

// ---------------------------------------------------------------------------
// declarations

void Analyzer::run(const Unit* unit)
{
    unit_ = unit;
    model_.program.name = unit->unitKind == UnitKind::fragment ? "" : unit->name;
    model_.program.type = types_.addProgram(&model_.program);
    unitOverflow_ = overflowFrom(unit, OverflowMode::trap, {"overflow"});
    for (const Import* import : unit->imports) {
        unsupported(import, "importing modules ('import " + import->name + "')", "phase 7");
    }

    collect(unit);
    for (const auto& record : model_.records) {
        resolveRecord(record.get());
    }
    for (MessageInfo* message : model_.program.messages) {
        ensureChecked(message);
    }

    if (unit->unitKind == UnitKind::program && model_.program.main == nullptr) {
        error(unit->nameRange, "sema.noMain",
              "program '" + unit->name + "' has no entry point; add 'extends " + unit->name +
                  " { @[static] msg main: args:array [int32] { ... } }'");
    }
}

void Analyzer::collect(const Unit* unit)
{
    for (const Node* item : unit->items) {
        if (auto record = nodeCast<RecordDecl>(item)) {
            declareRecord(record);
        }
    }
    for (const Node* item : unit->items) {
        switch (item->kind) {
        case NodeKind::recordDecl:
        case NodeKind::error:
        case NodeKind::messageDecl:
            break;
        case NodeKind::extendsDecl: {
            auto decl = static_cast<const ExtendsDecl*>(item);
            auto target = nodeCast<TypeName>(decl->target);
            std::string name = target != nullptr ? target->name : "";
            if (!model_.program.name.empty() && name == model_.program.name) {
                declareProgramMessages(decl);
            }
            else if (model_.recordNamed(name) != nullptr) {
                unsupported(decl->target, "messages on records ('extends " + name + "')",
                            "a later phase; whether records get messages at all is still an open design question");
            }
            else {
                error(decl->target, "sema.unknownType", "'extends " + name + "' names neither this program nor a known type");
            }
            break;
        }
        case NodeKind::classDecl:
            unsupported(item, "classes", "phase 4");
            break;
        case NodeKind::enumDecl:
            unsupported(item, "enums", "phase 5");
            break;
        case NodeKind::typeAliasDecl:
            unsupported(item, "named types and subranges", "phase 5");
            break;
        case NodeKind::namespaceDecl:
            unsupported(item, "namespaces", "phase 7");
            break;
        default:
            unsupported(item, "code and variables at unit level", "a later phase; put the code in a message");
            break;
        }
    }
}

void Analyzer::declareRecord(const RecordDecl* decl)
{
    if (isTypeName(decl->name) || decl->name == model_.program.name) {
        error(decl->nameRange, "sema.redeclared", "'" + decl->name + "' is already declared");
        return;
    }
    auto record = std::make_unique<RecordInfo>();
    record->name = decl->name;
    record->decl = decl;
    for (const Attribute* attribute : decl->attributes) {
        if (attribute->name == "packed" && attribute->value == nullptr) {
            record->packed = true;
        }
        else {
            unsupported(attribute, "the record attribute '@[" + attribute->name + "]'", "phase 9");
        }
    }
    record->type = types_.addRecord(record.get());
    model_.records.push_back(std::move(record));
}

void Analyzer::resolveRecord(RecordInfo* record)
{
    if (record->resolved) {
        return;
    }
    if (record->resolving) {
        error(record->decl->nameRange, "sema.recursiveRecord", "record '" + record->name + "' contains itself");
        return;
    }
    record->resolving = true;
    const RecordDecl* decl = record->decl;
    if (decl->base != nullptr) {
        const Type* base = resolveType(decl->base);
        if (base->kind == TypeKind::record) {
            resolveRecord(base->record);
            record->fields = base->record->fields;
        }
        else if (base->kind != TypeKind::error) {
            error(decl->base, "sema.invalidBase", "a record can only inherit another record, not '" + typeName(base) + "'");
        }
    }
    for (const Node* node : decl->fields) {
        auto field = nodeCast<FieldDecl>(node);
        if (field == nullptr) {
            continue;
        }
        if (record->field(field->name) != nullptr) {
            error(field->nameRange, "sema.redeclared", "record '" + record->name + "' already has a field '" + field->name + "'");
            continue;
        }
        for (const Attribute* attribute : field->attributes) {
            unsupported(attribute, "the field attribute '@[" + attribute->name + "]'", "phase 9");
        }
        if (field->initializer != nullptr) {
            unsupported(field->initializer, "default values for record fields", "a later phase");
        }
        const Type* type = resolveType(field->type);
        requireValueType(field->type, type);
        if (rejectEscapingBlock(field->type, type, "a record field")) {
            type = types_.error();
        }
        if (type->kind == TypeKind::record) {
            resolveRecord(type->record);
        }
        FieldInfo info;
        info.name = field->name;
        info.type = type;
        info.index = static_cast<uint32_t>(record->fields.size());
        info.decl = field;
        record->fields.push_back(info);
    }
    record->resolving = false;
    record->resolved = true;
}

void Analyzer::declareProgramMessages(const ExtendsDecl* decl)
{
    for (const Node* protocol : decl->protocols) {
        unsupported(protocol, "protocols", "phase 4");
    }
    for (const Node* member : decl->members) {
        if (auto message = nodeCast<MessageDecl>(member)) {
            declareMessage(message);
        }
    }
}

void Analyzer::declareMessage(const MessageDecl* decl)
{
    if (!hasAttribute(decl, "static")) {
        error(decl->selectorRange, "sema.programMessageNotStatic",
              "messages on the program are class-side; mark '" + decl->selector + "' with @[static]");
    }
    if (decl->body == nullptr) {
        error(decl->selectorRange, "sema.missingBody", "'" + decl->selector + "' needs a body; only protocols declare messages without one");
    }
    if (model_.program.bySelector.count(decl->selector) != 0) {
        error(decl->selectorRange, "sema.redeclared", "'" + decl->selector + "' is already declared on '" + model_.program.name + "'");
        return;
    }

    auto message = std::make_unique<MessageInfo>();
    message->selector = decl->selector;
    message->decl = decl;
    message->symbolName = "bob." + model_.program.name + "." + decl->selector;
    message->overflow = overflowFrom(decl, unitOverflow_, {"overflow", "static"});
    message->isMain = decl->selector == "main" || decl->selector == "main:";

    for (const Node* node : decl->params) {
        auto param = static_cast<const MessageParam*>(node);
        const Type* type = resolveType(param->type);
        if (type->kind == TypeKind::blockToInfer) {
            type = types_.pendingBlock();
        }
        bool mainArgs = message->isMain && type->kind == TypeKind::placeholder && type->name == "array";
        if (!mainArgs) {
            requireValueType(param->type, type);
        }
        auto local = std::make_unique<LocalInfo>();
        local->name = param->name;
        local->type = type;
        local->decl = param;
        local->isParameter = true;
        local->readOnly = true;
        local->index = static_cast<uint32_t>(message->locals.size());
        message->params.push_back(local.get());
        message->locals.push_back(local.get());
        model_.localRefs[param->id] = local.get();
        model_.locals.push_back(std::move(local));
    }

    message->returnType = decl->returnType != nullptr ? resolveType(decl->returnType) : types_.voidType();
    if (decl->returnType != nullptr) {
        requireValueType(decl->returnType, message->returnType);
        if (rejectEscapingBlock(decl->returnType, message->returnType, "a message's return type")) {
            message->returnType = types_.error();
        }
    }

    if (message->isMain) {
        if (message->returnType != types_.int32()) {
            error(decl->selectorRange, "sema.invalidMain", "'main' must return [int32], the process exit code");
        }
        if (decl->selector == "main:" && !(message->params[0]->type->kind == TypeKind::placeholder &&
                                           message->params[0]->type->name == "array")) {
            error(decl->params[0]->range, "sema.invalidMain", "the parameter of 'main:' must be 'args:array'");
        }
        model_.program.main = message.get();
    }

    model_.program.bySelector[message->selector] = message.get();
    model_.program.messages.push_back(message.get());
    model_.messages.push_back(std::move(message));
}

const Type* Analyzer::resolveType(const Node* node)
{
    if (node == nullptr) {
        return types_.voidType();
    }
    if (auto name = nodeCast<TypeName>(node)) {
        if (const Type* builtin = types_.builtin(name->name)) {
            return builtin;
        }
        if (const RecordInfo* record = model_.recordNamed(name->name)) {
            return record->type;
        }
        error(node, "sema.unknownType", "unknown type '" + name->name + "'");
        return types_.error();
    }
    if (auto message = nodeCast<TypeMessage>(node)) {
        auto receiver = nodeCast<TypeName>(message->receiver);
        if (receiver != nullptr && receiver->name == "block") {
            return resolveBlockType(message);
        }
    }
    switch (node->kind) {
    case NodeKind::optionalType:
        unsupported(node, "optional types", "phase 4");
        break;
    case NodeKind::tupleType:
        unsupported(node, "tuple types (several return values)", "a later phase");
        break;
    case NodeKind::typeMessage:
        unsupported(node, "type messages such as 'array of:' and subranges", "phase 5");
        break;
    default:
        error(node, "sema.expectedType", "expected a type");
        break;
    }
    return types_.error();
}

// `block takes: [T1, T2] returns: R`, `block takes: [T]`, `block returns: R`.
const Type* Analyzer::resolveBlockType(const TypeMessage* message)
{
    std::vector<const Type*> params;
    const Type* result = types_.voidType();
    if (message->selector != "takes:returns:" && message->selector != "takes:" && message->selector != "returns:") {
        error(message, "sema.invalidBlockType", "a block type is written 'block takes: [T, ...] returns: R'");
        return types_.error();
    }
    size_t argument = 0;
    if (message->selector.rfind("takes:", 0) == 0) {
        const Node* takes = message->arguments[argument++];
        if (auto tuple = nodeCast<TupleType>(takes)) {
            for (const Node* element : tuple->elements) {
                params.push_back(resolveType(static_cast<const TupleTypeElement*>(element)->type));
            }
        }
        else {
            params.push_back(resolveType(takes));
        }
    }
    if (argument < message->arguments.size()) {
        result = resolveType(message->arguments[argument]);
    }
    for (const Type* param : params) {
        if (param == types_.error()) {
            return types_.error();
        }
        if (!isValueType(param)) {
            error(message, "sema.invalidBlockType", "block parameters need value types, not '" + typeName(param) + "'");
            return types_.error();
        }
    }
    if (result == types_.error()) {
        return types_.error();
    }
    return types_.block(std::move(params), result);
}

bool Analyzer::requireValueType(const Node* node, const Type* type)
{
    if (type->kind == TypeKind::placeholder) {
        unsupported(node, "the type '" + type->name + "'", type->name == "int128" || type->name == "uint128" ? "a later phase" : "phase 5");
        return false;
    }
    return true;
}

bool Analyzer::rejectEscapingBlock(const Node* node, const Type* type, const char* where)
{
    if (isBlock(type) || type->kind == TypeKind::blockToInfer) {
        unsupported(node, std::string("a block as ") + where + " (the block would outlive the frame that created it)",
                    "phase 8, escaping closures");
        return true;
    }
    return false;
}

OverflowMode Analyzer::overflowFrom(const Node* node, OverflowMode inherited, std::initializer_list<std::string_view> allowed)
{
    OverflowMode mode = inherited;
    for (const Attribute* attribute : node->attributes) {
        if (std::find(allowed.begin(), allowed.end(), attribute->name) == allowed.end()) {
            unsupported(attribute, "the attribute '@[" + attribute->name + "]' here", "a later phase");
            continue;
        }
        if (attribute->name != "overflow") {
            continue;
        }
        auto value = nodeCast<Identifier>(attribute->value);
        if (value != nullptr && value->name == "trap") {
            mode = OverflowMode::trap;
        }
        else if (value != nullptr && value->name == "wrap") {
            mode = OverflowMode::wrap;
        }
        else if (value != nullptr && value->name == "saturate") {
            mode = OverflowMode::saturate;
        }
        else {
            error(attribute, "sema.invalidAttribute", "'@[overflow: ...]' takes 'trap', 'wrap' or 'saturate'");
        }
    }
    return mode;
}

// ---------------------------------------------------------------------------
// message bodies

// Messages are checked on demand so a caller sees the inferred types of a
// callee's bare `block` parameters; a cycle leaves them to be annotated.
void Analyzer::ensureChecked(MessageInfo* message)
{
    int& state = checkState_[message];
    if (state != 0) {
        return;
    }
    state = 1;
    FunctionContext savedContext = context_;
    auto savedScopes = std::move(scopes_);
    auto savedOverflow = std::move(overflow_);
    scopes_.clear();
    overflow_.clear();

    checkMessage(message);

    context_ = savedContext;
    scopes_ = std::move(savedScopes);
    overflow_ = std::move(savedOverflow);
    state = 2;
}

void Analyzer::checkMessage(MessageInfo* message)
{
    context_ = FunctionContext{message, nullptr, &message->locals, true, 0};
    overflow_ = {message->overflow};
    pushScope();
    for (LocalInfo* param : message->params) {
        scopes_.back()[param->name] = param;
    }
    auto body = nodeCast<StatementGroup>(message->decl->body);
    if (body != nullptr) {
        checkStatements(body->statements);
        if (message->returnType != types_.voidType() && message->returnType != types_.error() && !alwaysReturns(body->statements)) {
            error(message->decl->selectorRange, "sema.missingReturn",
                  "'" + message->selector + "' doesn't return a value on every path");
        }
    }
    popScope();
    finalizeInferredBlocks(message);
}

void Analyzer::finalizeInferredBlocks(MessageInfo* message)
{
    for (LocalInfo* param : message->params) {
        if (!isBlock(param->type) || !param->type->pending) {
            continue;
        }
        Type* type = const_cast<Type*>(param->type);
        bool complete = type->arityKnown;
        for (const Type* p : type->params) {
            complete = complete && p != nullptr;
        }
        if (!complete) {
            error(static_cast<const MessageParam*>(param->decl)->nameRange, "sema.cannotInferBlock",
                  "the type of block parameter '" + param->name +
                      "' can't be inferred from how it's used; write it as '" + param->name + ":[block takes: [T] returns: R]'");
            param->type = types_.error();
            continue;
        }
        if (type->result == nullptr) {
            type->result = types_.voidType();
        }
        type->pending = false;
        type->name = blockTypeName(type);
    }
}

void Analyzer::checkStatements(const std::vector<Node*>& statements)
{
    for (const Node* statement : statements) {
        checkStatement(statement);
    }
}

void Analyzer::checkGroup(const Node* node)
{
    auto group = nodeCast<StatementGroup>(node);
    if (group == nullptr) {
        return;
    }
    overflow_.push_back(overflowFrom(group, overflow(), {"overflow"}));
    pushScope();
    checkStatements(group->statements);
    popScope();
    overflow_.pop_back();
}

void Analyzer::checkCondition(const Node* condition, const char* statement)
{
    const Type* type = check(condition, types_.boolType());
    if (type != types_.boolType() && type != types_.error()) {
        error(condition, "sema.conditionNotBool",
              std::string("the condition of '") + statement + "' must be a bool, not '" + typeName(type) + "'");
    }
}

void Analyzer::checkStatement(const Node* statement)
{
    if (statement->kind != NodeKind::statementGroup) {
        for (const Attribute* attribute : statement->attributes) {
            unsupported(attribute, "the attribute '@[" + attribute->name + "]' on a statement", "a later phase");
        }
    }
    switch (statement->kind) {
    case NodeKind::variableDecl: {
        auto decl = static_cast<const VariableDecl*>(statement);
        const Type* type = resolveType(decl->type);
        requireValueType(decl->type, type);
        if (type->kind == TypeKind::blockToInfer) {
            if (decl->initializer == nullptr) {
                error(decl->type, "sema.cannotInferBlock",
                      "a bare 'block' variable needs an initializer to take its type from, or write 'block takes: [T] returns: R'");
                type = types_.error();
            }
            else {
                type = check(decl->initializer, nullptr);
                if (!isBlock(type) && type != types_.error()) {
                    error(decl->initializer, "sema.typeMismatch", "'" + decl->name + "' is a block, but this is a '" + typeName(type) + "'");
                    type = types_.error();
                }
            }
        }
        else if (decl->initializer != nullptr) {
            const Type* initType = check(decl->initializer, type);
            requireAssignable(decl->initializer, initType, type, "initialize");
        }
        LocalInfo* local = declareLocal(decl->name, type, decl, decl->nameRange);
        model_.localRefs[decl->id] = local;
        break;
    }
    case NodeKind::assignment:
        checkAssignment(static_cast<const Assignment*>(statement));
        break;
    case NodeKind::expressionStmt:
        check(static_cast<const ExpressionStmt*>(statement)->expression, nullptr);
        break;
    case NodeKind::returnStmt:
        checkReturn(static_cast<const ReturnStmt*>(statement));
        break;
    case NodeKind::ifStmt: {
        auto ifStmt = static_cast<const IfStmt*>(statement);
        checkCondition(ifStmt->condition, "if");
        checkGroup(ifStmt->thenBody);
        if (ifStmt->elseBody != nullptr && ifStmt->elseBody->kind == NodeKind::ifStmt) {
            checkStatement(ifStmt->elseBody);
        }
        else {
            checkGroup(ifStmt->elseBody);
        }
        break;
    }
    case NodeKind::whileStmt: {
        auto whileStmt = static_cast<const WhileStmt*>(statement);
        checkCondition(whileStmt->condition, "while");
        ++context_.loopDepth;
        checkGroup(whileStmt->body);
        --context_.loopDepth;
        break;
    }
    case NodeKind::forStmt:
        checkFor(static_cast<const ForStmt*>(statement));
        break;
    case NodeKind::loopStmt:
        ++context_.loopDepth;
        checkGroup(static_cast<const LoopStmt*>(statement)->body);
        --context_.loopDepth;
        break;
    case NodeKind::breakStmt:
    case NodeKind::continueStmt:
        if (context_.loopDepth == 0) {
            error(statement, "sema.jumpOutsideLoop",
                  std::string("'") + (statement->kind == NodeKind::breakStmt ? "break" : "continue") + "' is only valid inside a loop");
        }
        break;
    case NodeKind::caseStmt:
        checkCase(static_cast<const CaseStmt*>(statement));
        break;
    case NodeKind::statementGroup:
        checkGroup(statement);
        break;
    case NodeKind::error:
        break;
    default:
        error(statement, "sema.notAStatement", std::string("unexpected ") + nodeKindName(statement->kind) + " in a statement list");
        break;
    }
}

void Analyzer::checkAssignment(const Assignment* assignment)
{
    if (auto identifier = nodeCast<Identifier>(assignment->target)) {
        if (LocalInfo* local = lookupLocal(identifier->name)) {
            model_.localRefs[identifier->id] = local;
            remember(identifier, local->type);
            if (local->readOnly) {
                error(identifier, "sema.assignToReadOnly",
                      "'" + identifier->name + "' is " + (local->isParameter ? "a parameter" : "a loop variable") + " and can't be assigned");
            }
            const Type* valueType = check(assignment->value, local->type);
            requireAssignable(assignment->value, valueType, local->type, "assign");
            return;
        }
        if (isTypeName(identifier->name) || (!model_.program.name.empty() && identifier->name == model_.program.name)) {
            error(identifier, "sema.assignToType", "'" + identifier->name + "' is a type and can't be assigned");
            check(assignment->value, nullptr);
            return;
        }
        // Implicit declaration: the value is checked before the name exists, so
        // `count := count + 1` with an unknown `count` is an undeclared-identifier error.
        const Type* valueType = check(assignment->value, nullptr);
        if (valueType != types_.error() && !isValueType(valueType)) {
            error(assignment->value, "sema.cannotInfer",
                  "can't infer the type of '" + identifier->name + "' from a value of type '" + typeName(valueType) + "'");
            valueType = types_.error();
        }
        LocalInfo* local = declareLocal(identifier->name, valueType, assignment, identifier->range);
        model_.implicitDeclarations.insert(assignment->id);
        model_.localRefs[assignment->id] = local;
        model_.localRefs[identifier->id] = local;
        remember(identifier, valueType);
        return;
    }

    const Type* targetType = check(assignment->target, nullptr);
    if (assignment->target->kind == NodeKind::fieldAccess) {
        const LocalInfo* root = rootLocal(assignment->target);
        if (root == nullptr && targetType != types_.error()) {
            error(assignment->target, "sema.notAssignable", "only fields of variables can be assigned");
        }
        else if (root != nullptr && root->readOnly) {
            error(assignment->target, "sema.assignToReadOnly", "'" + root->name + "' is read-only, so its fields can't be assigned");
        }
    }
    const Type* valueType = check(assignment->value, targetType);
    requireAssignable(assignment->value, valueType, targetType, "assign");
}

void Analyzer::checkReturn(const ReturnStmt* statement)
{
    if (!context_.allowReturn) {
        error(statement, "sema.returnInBlock",
              "'return' inside a block that isn't inlined would have to unwind the stack; make the value the block's last "
              "expression instead (only blocks given directly to ifTrue:, to:do:, timesRepeat: ... may return)");
        for (const Node* value : statement->values) {
            check(value, nullptr);
        }
        return;
    }
    const Type* returnType = context_.message->returnType;
    if (statement->values.size() > 1) {
        unsupported(statement, "returning several values", "a later phase");
        return;
    }
    if (statement->values.empty()) {
        if (returnType != types_.voidType() && returnType != types_.error()) {
            error(statement, "sema.missingReturnValue",
                  "'" + context_.message->selector + "' must return a value of type '" + typeName(returnType) + "'");
        }
        return;
    }
    if (returnType == types_.voidType()) {
        error(statement->values[0], "sema.unexpectedReturnValue",
              "'" + context_.message->selector + "' has no return type; add one such as [int32] or remove the value");
        check(statement->values[0], nullptr);
        return;
    }
    const Type* valueType = check(statement->values[0], returnType);
    requireAssignable(statement->values[0], valueType, returnType, "return");
}

void Analyzer::checkFor(const ForStmt* statement)
{
    pushScope();
    const Type* declared = statement->variableType != nullptr ? resolveType(statement->variableType) : nullptr;
    const Type* elementType = types_.error();
    auto interval = nodeCast<KeywordSend>(statement->iterable);
    if (interval == nullptr || interval->selector != "to:") {
        check(statement->iterable, nullptr);
        unsupported(statement->iterable, "iterating anything other than an integer interval 'a to: b'", "phase 5");
    }
    else {
        const Type* common = checkOperandPair(interval->receiver, interval->arguments[0], declared, "the bounds of the interval", interval);
        if (common != types_.error() && !isInteger(common)) {
            error(interval, "sema.invalidInterval", "the bounds of an interval must be integers, not '" + typeName(common) + "'");
            common = types_.error();
        }
        elementType = common;
        if (declared != nullptr && common != types_.error()) {
            if (requireAssignable(interval, common, declared, "iterate")) {
                elementType = declared;
            }
        }
        addSend(interval, SendKind::interval, "to:", elementType, elementType);
        remember(interval, elementType);
    }
    LocalInfo* local = declareLocal(statement->variable, elementType, statement, statement->variableRange);
    local->readOnly = true;
    model_.localRefs[statement->id] = local;
    ++context_.loopDepth;
    checkGroup(statement->body);
    --context_.loopDepth;
    popScope();
}

void Analyzer::checkCase(const CaseStmt* statement)
{
    const Type* subject = check(statement->subject, nullptr);
    bool usable = isInteger(subject);
    if (!usable && subject != types_.error()) {
        error(statement->subject, "sema.invalidCaseSubject",
              "'case' works on integers for now (enums and unions come in phase 5), not on '" + typeName(subject) + "'");
    }

    std::vector<CaseRange> seen;
    auto overlaps = [&](const CaseRange& a, const CaseRange& b) {
        if (subject->isSigned) {
            return static_cast<int64_t>(a.low) <= static_cast<int64_t>(b.high) && static_cast<int64_t>(b.low) <= static_cast<int64_t>(a.high);
        }
        return a.low <= b.high && b.low <= a.high;
    };
    auto constantOf = [&](const Node* node, uint64_t& bits) {
        auto literal = nodeCast<IntegerLiteral>(node);
        if (literal == nullptr) {
            error(node, "sema.caseLabelNotConstant", "case labels must be integer literals or ranges such as '(1 to: 9)'");
            return false;
        }
        remember(literal, subject);
        if (!integerFits(literal->value, literal->negative, subject)) {
            error(literal, "sema.literalOutOfRange", "case label " + literal->text + " doesn't fit in '" + typeName(subject) + "'");
            return false;
        }
        bits = literal->negative ? (~literal->value + 1) : literal->value;
        return true;
    };

    for (const Node* node : statement->entries) {
        auto entry = nodeCast<CaseEntry>(node);
        if (entry == nullptr) {
            continue;
        }
        std::vector<CaseRange> ranges;
        for (const Node* label : entry->labels) {
            if (!usable) {
                break;
            }
            CaseRange range;
            auto interval = nodeCast<KeywordSend>(label);
            if (interval != nullptr && interval->selector == "to:") {
                if (!constantOf(interval->receiver, range.low) || !constantOf(interval->arguments[0], range.high)) {
                    continue;
                }
                bool reversed = subject->isSigned ? static_cast<int64_t>(range.low) > static_cast<int64_t>(range.high) : range.low > range.high;
                if (reversed) {
                    error(label, "sema.emptyCaseRange", "the case range is empty: its first value is greater than its last");
                    continue;
                }
                remember(interval, subject);
            }
            else {
                if (!constantOf(label, range.low)) {
                    continue;
                }
                range.high = range.low;
            }
            bool clash = false;
            for (const CaseRange& other : seen) {
                clash = clash || overlaps(range, other);
            }
            if (clash) {
                error(label, "sema.overlappingCaseLabels", "this case label overlaps an earlier one");
                continue;
            }
            seen.push_back(range);
            ranges.push_back(range);
        }
        model_.caseRanges[entry->id] = ranges;

        auto body = nodeCast<BlockLiteral>(entry->body);
        if (body != nullptr) {
            if (!body->params.empty()) {
                unsupported(body->params.front(), "binding the matched value in a case entry", "phase 5 (unions)");
            }
            pushScope();
            checkStatements(body->statements);
            popScope();
        }
    }
    if (statement->elseBody == nullptr) {
        if (usable) {
            error(statement, "sema.caseNotExhaustive", "a 'case' over an integer needs an 'else' (only enums can be covered completely)");
        }
    }
    else {
        checkGroup(statement->elseBody);
    }
}

// ---------------------------------------------------------------------------
// expressions

const Type* Analyzer::remember(const Node* node, const Type* type)
{
    model_.expressionTypes[node->id] = type;
    return type;
}

void Analyzer::addSend(const Node* node, SendKind kind, std::string op, const Type* operandType, const Type* resultType)
{
    SendInfo info;
    info.kind = kind;
    info.op = std::move(op);
    info.operandType = operandType;
    info.resultType = resultType;
    info.overflow = overflow();
    model_.sends[node->id] = info;
}

bool Analyzer::isUntypedLiteral(const Node* node) const
{
    if (auto integer = nodeCast<IntegerLiteral>(node)) {
        return integer->suffix.empty();
    }
    if (auto real = nodeCast<RealLiteral>(node)) {
        return real->suffix.empty();
    }
    if (auto binary = nodeCast<BinarySend>(node)) {
        return isArithmeticOperator(binary->selector) && isUntypedLiteral(binary->receiver) && isUntypedLiteral(binary->argument);
    }
    if (auto unary = nodeCast<UnarySend>(node)) {
        return (unary->selector == "negated" || unary->selector == "abs") && isUntypedLiteral(unary->receiver);
    }
    return false;
}

bool Analyzer::requireAssignable(const Node* node, const Type* from, const Type* to, const char* verb)
{
    if (from == types_.error() || to == types_.error() || to == nullptr) {
        return true;
    }
    if (isBlock(from) && from->pending && isBlock(to)) {
        return unify(from, to, node);
    }
    if (canWiden(from, to) && isValueType(from)) {
        return true;
    }
    std::string message = std::string("can't ") + verb + " a value of type '" + typeName(from) + "' as '" + typeName(to) + "'";
    if (isNumeric(from) && isNumeric(to)) {
        message += "; convert it explicitly with 'as" + capitalize(to->name) + "'";
    }
    error(node, "sema.typeMismatch", message);
    return false;
}

const Type* Analyzer::check(const Node* node, const Type* expected)
{
    const Type* type = types_.error();
    switch (node->kind) {
    case NodeKind::integerLiteral:
        type = checkIntegerLiteral(static_cast<const IntegerLiteral*>(node), expected);
        break;
    case NodeKind::realLiteral:
        type = checkRealLiteral(static_cast<const RealLiteral*>(node), expected);
        break;
    case NodeKind::boolLiteral:
        type = types_.boolType();
        break;
    case NodeKind::nilLiteral:
        unsupported(node, "'nil' (optional types)", "phase 4");
        break;
    case NodeKind::stringLiteral:
    case NodeKind::charLiteral:
        unsupported(node, "strings and characters", "phase 5");
        break;
    case NodeKind::identifier:
        type = checkIdentifier(static_cast<const Identifier*>(node));
        break;
    case NodeKind::selfExpr:
        type = model_.program.type;
        break;
    case NodeKind::superExpr:
        unsupported(node, "'super'", "phase 4");
        break;
    case NodeKind::fieldAccess:
        type = checkFieldAccess(static_cast<const FieldAccess*>(node));
        break;
    case NodeKind::unarySend:
        type = checkUnary(static_cast<const UnarySend*>(node), expected);
        break;
    case NodeKind::binarySend:
        type = checkBinary(static_cast<const BinarySend*>(node), expected);
        break;
    case NodeKind::keywordSend:
        type = checkKeyword(static_cast<const KeywordSend*>(node), expected);
        break;
    case NodeKind::arrayLiteral:
        unsupported(node, "array literals", "phase 5");
        break;
    case NodeKind::blockLiteral:
        type = checkBlockLiteral(static_cast<const BlockLiteral*>(node), isBlock(expected) && !expected->pending ? expected : nullptr);
        break;
    case NodeKind::attributedExpr: {
        auto attributed = static_cast<const AttributedExpr*>(node);
        overflow_.push_back(overflowFrom(attributed, overflow(), {"overflow"}));
        type = check(attributed->expression, expected);
        overflow_.pop_back();
        break;
    }
    case NodeKind::error:
        break;
    default:
        error(node, "sema.notAnExpression", std::string("expected an expression, found ") + nodeKindName(node->kind));
        break;
    }
    return remember(node, type);
}

const Type* Analyzer::checkIntegerLiteral(const IntegerLiteral* literal, const Type* expected)
{
    const Type* target = nullptr;
    if (!literal->suffix.empty()) {
        const std::string& s = literal->suffix;
        if (s == "i128" || s == "u128") {
            unsupported(literal, "128-bit integers", "a later phase");
            return types_.error();
        }
        uint32_t bits = static_cast<uint32_t>(std::stoul(s.substr(1)));
        target = types_.integer(bits, s[0] == 'i');
    }
    else if (isInteger(expected)) {
        target = expected;
    }
    else if (isReal(expected)) {
        return expected;
    }
    else {
        target = types_.int32();
    }
    if (!integerFits(literal->value, literal->negative, target)) {
        std::string message = "the integer literal " + literal->text + " doesn't fit in '" + typeName(target) + "'";
        if (literal->suffix.empty() && !isInteger(expected)) {
            message += "; give it a type with a suffix such as 10u64 or a typed declaration";
        }
        error(literal, "sema.literalOutOfRange", message);
    }
    return target;
}

const Type* Analyzer::checkRealLiteral(const RealLiteral* literal, const Type* expected)
{
    if (!literal->suffix.empty()) {
        return types_.real(literal->suffix == "r32" ? 32 : 64);
    }
    if (isReal(expected)) {
        return expected;
    }
    if (isInteger(expected)) {
        error(literal, "sema.typeMismatch",
              "a real literal can't be used as '" + typeName(expected) + "'; convert it with 'as" + capitalize(expected->name) + "'");
        return types_.error();
    }
    return types_.real64();
}

const Type* Analyzer::checkIdentifier(const Identifier* identifier)
{
    if (LocalInfo* local = lookupLocal(identifier->name)) {
        model_.localRefs[identifier->id] = local;
        if (local->type->kind == TypeKind::placeholder) {
            unsupported(identifier, "using a value of type '" + local->type->name + "'", "phase 5");
            return types_.error();
        }
        return local->type;
    }
    if (!model_.program.name.empty() && identifier->name == model_.program.name) {
        return model_.program.type;
    }
    if (const RecordInfo* record = model_.recordNamed(identifier->name)) {
        return types_.metaOf(record->type);
    }
    if (const Type* builtin = types_.builtin(identifier->name)) {
        return types_.metaOf(builtin);
    }
    error(identifier, "sema.undeclared", "undeclared identifier '" + identifier->name + "'");
    return types_.error();
}

const Type* Analyzer::checkFieldAccess(const FieldAccess* access)
{
    const Type* receiver = check(access->receiver, nullptr);
    if (receiver == types_.error()) {
        return types_.error();
    }
    if (receiver->kind != TypeKind::record) {
        error(access->fieldRange, "sema.notARecord", "'." + access->field + "' needs a record, but this is a '" + typeName(receiver) + "'");
        return types_.error();
    }
    const FieldInfo* field = receiver->record->field(access->field);
    if (field == nullptr) {
        error(access->fieldRange, "sema.unknownField", "record '" + receiver->name + "' has no field '" + access->field + "'");
        return types_.error();
    }
    model_.fieldRefs[access->id] = field;
    return field->type;
}

const Type* Analyzer::checkUnary(const UnarySend* send, const Type* expected)
{
    const Type* receiver = check(send->receiver, nullptr);
    if (receiver == types_.error()) {
        return types_.error();
    }
    const std::string& selector = send->selector;
    if (isBlock(receiver) && selector == "value") {
        return checkBlockInvoke(send, receiver, {}, expected);
    }
    if (receiver->kind == TypeKind::meta && receiver->program != nullptr) {
        auto it = receiver->program->bySelector.find(selector);
        if (it == receiver->program->bySelector.end()) {
            error(send->selectorRange, "sema.unknownMessage", "'" + receiver->name + "' has no message '" + selector + "'");
            return types_.error();
        }
        addSend(send, SendKind::staticCall, selector, nullptr, it->second->returnType);
        model_.sends[send->id].target = it->second;
        return it->second->returnType;
    }
    if (isNumeric(receiver)) {
        if (selector == "negated") {
            if (!receiver->isSigned && isInteger(receiver)) {
                error(send->selectorRange, "sema.unsignedNegate", "an unsigned '" + receiver->name + "' can't be negated");
                return types_.error();
            }
            addSend(send, SendKind::negate, selector, receiver, receiver);
            return receiver;
        }
        if (selector == "abs") {
            addSend(send, SendKind::absolute, selector, receiver, receiver);
            return receiver;
        }
        if (selector.rfind("as", 0) == 0) {
            std::string targetName = selector.substr(2);
            if (targetName.rfind("UInt", 0) == 0) {
                targetName = "uint" + targetName.substr(4);
            }
            else if (!targetName.empty()) {
                targetName[0] = static_cast<char>(targetName[0] - 'A' + 'a');
            }
            const Type* target = types_.builtin(targetName);
            if (isNumeric(target)) {
                addSend(send, SendKind::conversion, selector, receiver, target);
                return target;
            }
        }
    }
    if (receiver == types_.boolType() && selector == "not") {
        addSend(send, SendKind::boolNot, selector, receiver, receiver);
        return receiver;
    }
    if (receiver->kind == TypeKind::meta && receiver->instance != nullptr && receiver->instance->kind == TypeKind::record &&
        selector == "new") {
        error(send->selectorRange, "sema.unknownMessage",
              "records aren't created with 'new': declare 'p: " + receiver->instance->name + ";' or use '" +
                  receiver->instance->name + " " + receiver->instance->record->constructorSelector() + " …'");
        return types_.error();
    }
    error(send->selectorRange, "sema.unknownMessage", "'" + typeName(receiver) + "' doesn't understand '" + selector + "'");
    return types_.error();
}

const Type* Analyzer::checkOperandPair(const Node* lhs, const Node* rhs, const Type* expected, const char* what, const Node* site)
{
    const Type* left = nullptr;
    const Type* right = nullptr;
    bool leftUntyped = isUntypedLiteral(lhs);
    bool rightUntyped = isUntypedLiteral(rhs);
    if (leftUntyped && !rightUntyped) {
        right = check(rhs, expected);
        left = check(lhs, isNumeric(right) ? right : expected);
    }
    else if (rightUntyped && !leftUntyped) {
        left = check(lhs, expected);
        right = check(rhs, isNumeric(left) ? left : expected);
    }
    else if (leftUntyped && rightUntyped) {
        left = check(lhs, expected);
        right = check(rhs, isNumeric(left) ? left : expected);
    }
    else {
        left = check(lhs, nullptr);
        right = check(rhs, nullptr);
    }
    if (left == types_.error() || right == types_.error()) {
        return types_.error();
    }
    const Type* common = commonType(left, right);
    if (common == nullptr || !isValueType(common) || isBlock(common)) {
        std::string message = std::string("mismatched types in ") + what + ": '" + typeName(left) + "' and '" + typeName(right) + "'";
        if (isNumeric(left) && isNumeric(right)) {
            message += "; convert one side explicitly, for example with 'as" + capitalize(left->name) + "'";
        }
        error(site, "sema.typeMismatch", message);
        return types_.error();
    }
    return common;
}

const Type* Analyzer::checkBinary(const BinarySend* send, const Type* expected)
{
    const std::string& op = send->selector;
    if (op == "and" || op == "or") {
        const Type* left = check(send->receiver, types_.boolType());
        const Type* right = check(send->argument, types_.boolType());
        if ((left != types_.boolType() && left != types_.error()) || (right != types_.boolType() && right != types_.error())) {
            error(send->selectorRange, "sema.typeMismatch", "'" + op + "' needs bool operands");
            return types_.error();
        }
        addSend(send, op == "and" ? SendKind::logicalAnd : SendKind::logicalOr, op, types_.boolType(), types_.boolType());
        return types_.boolType();
    }

    if (op == "shl" || op == "shr") {
        const Type* value = check(send->receiver, isInteger(expected) ? expected : nullptr);
        const Type* amount = check(send->argument, isUntypedLiteral(send->argument) && isInteger(value) ? value : nullptr);
        if (value == types_.error() || amount == types_.error()) {
            return types_.error();
        }
        if (!isInteger(value) || !isInteger(amount)) {
            error(send->selectorRange, "sema.unknownMessage", "'" + op + "' needs an integer value and an integer shift amount");
            return types_.error();
        }
        addSend(send, SendKind::shift, op, value, value);
        return value;
    }

    bool comparison = isComparisonOperator(op);
    if (!comparison && !isArithmeticOperator(op) && !isBitwiseOperator(op)) {
        check(send->receiver, nullptr);
        check(send->argument, nullptr);
        error(send->selectorRange, "sema.unknownMessage", "there is no binary operator '" + op + "'");
        return types_.error();
    }
    const Type* common = checkOperandPair(send->receiver, send->argument, comparison ? nullptr : expected,
                                          ("'" + op + "'").c_str(), send);
    if (common == types_.error()) {
        return types_.error();
    }
    if (comparison) {
        bool ordered = op != "=" && op != "<>";
        if (!(isNumeric(common) || (!ordered && common == types_.boolType()))) {
            if (common->kind == TypeKind::record) {
                unsupported(send, "comparing records", "a later phase");
            }
            else {
                error(send->selectorRange, "sema.unknownMessage", "'" + typeName(common) + "' values can't be compared with '" + op + "'");
            }
            return types_.error();
        }
        addSend(send, SendKind::comparison, op, common, types_.boolType());
        return types_.boolType();
    }
    if (isBitwiseOperator(op)) {
        if (!isInteger(common)) {
            error(send->selectorRange, "sema.unknownMessage", "'" + op + "' needs integer operands, not '" + typeName(common) + "'");
            return types_.error();
        }
        addSend(send, SendKind::bitwise, op, common, common);
        return common;
    }
    if (!isNumeric(common)) {
        error(send->selectorRange, "sema.unknownMessage", "'" + op + "' needs numeric operands, not '" + typeName(common) + "'");
        return types_.error();
    }
    if (op == "%" && isReal(common)) {
        unsupported(send, "'%' on real numbers", "a later phase");
        return types_.error();
    }
    addSend(send, SendKind::arithmetic, op, common, common);
    return common;
}

void Analyzer::checkArgument(const Node* argument, const Type* parameterType)
{
    const Type* type = check(argument, parameterType);
    requireAssignable(argument, type, parameterType, "pass");
}

const Type* Analyzer::checkKeyword(const KeywordSend* send, const Type* expected)
{
    const std::string& selector = send->selector;
    SourceRange selectorRange = send->partRanges.empty() ? send->range : send->partRanges.front();
    const Type* receiver = check(send->receiver, nullptr);
    if (receiver == types_.error()) {
        for (const Node* argument : send->arguments) {
            check(argument, nullptr);
        }
        return types_.error();
    }

    if (receiver == types_.boolType() &&
        (selector == "ifTrue:" || selector == "ifFalse:" || selector == "ifTrue:ifFalse:" || selector == "ifFalse:ifTrue:")) {
        return checkInlinedIf(send, expected);
    }
    if (isInteger(receiver) && selector == "to:do:") {
        return checkInlinedToDo(send);
    }
    if (isInteger(receiver) && selector == "timesRepeat:") {
        return checkInlinedTimesRepeat(send, receiver);
    }
    if (isBlock(receiver) && valueSelectorArity(selector) > 0) {
        return checkBlockInvoke(send, receiver, send->arguments, expected);
    }

    if (receiver->kind == TypeKind::meta && receiver->program != nullptr) {
        auto it = receiver->program->bySelector.find(selector);
        if (it == receiver->program->bySelector.end()) {
            for (const Node* argument : send->arguments) {
                check(argument, nullptr);
            }
            error(selectorRange, "sema.unknownMessage", "'" + receiver->name + "' has no message '" + selector + "'");
            return types_.error();
        }
        MessageInfo* target = it->second;
        ensureChecked(target);
        for (size_t i = 0; i < send->arguments.size(); ++i) {
            const Type* parameterType = target->params[i]->type;
            if (isBlock(parameterType) && parameterType->pending) {
                error(send->arguments[i], "sema.cannotInferBlock",
                      "the block parameter '" + target->params[i]->name + "' of '" + target->selector +
                          "' is still being inferred (the messages call each other); annotate it with 'block takes: [T] returns: R'");
                check(send->arguments[i], nullptr);
                continue;
            }
            checkArgument(send->arguments[i], parameterType);
        }
        addSend(send, SendKind::staticCall, selector, nullptr, target->returnType);
        model_.sends[send->id].target = target;
        return target->returnType;
    }

    if (receiver->kind == TypeKind::meta && receiver->instance != nullptr && receiver->instance->kind == TypeKind::record) {
        const RecordInfo* record = receiver->instance->record;
        if (selector != record->constructorSelector()) {
            for (const Node* argument : send->arguments) {
                check(argument, nullptr);
            }
            error(selectorRange, "sema.unknownMessage",
                  "to create a '" + record->name + "' give every field in order: '" + record->name + " " + record->constructorSelector() + " …'");
            return types_.error();
        }
        for (size_t i = 0; i < send->arguments.size(); ++i) {
            checkArgument(send->arguments[i], record->fields[i].type);
        }
        addSend(send, SendKind::recordConstructor, selector, nullptr, record->type);
        model_.sends[send->id].record = record;
        return record->type;
    }

    if (isNumeric(receiver) && send->arguments.size() == 1) {
        const Node* argument = send->arguments[0];
        bool integerOnly = selector != "max:" && selector != "min:";
        SendKind kind = SendKind::minimum;
        if (selector == "max:") {
            kind = SendKind::maximum;
        }
        else if (selector == "wrapAdd:" || selector == "wrapSub:" || selector == "wrapMul:") {
            kind = SendKind::wrapArithmetic;
        }
        else if (selector == "satAdd:" || selector == "satSub:" || selector == "satMul:") {
            kind = SendKind::saturateArithmetic;
        }
        else if (selector != "min:") {
            kind = SendKind::staticCall;
        }

        if (kind != SendKind::staticCall) {
            const Type* argumentType = check(argument, receiver);
            if (argumentType == types_.error()) {
                return types_.error();
            }
            const Type* common = commonType(receiver, argumentType);
            if (common == nullptr) {
                error(send, "sema.typeMismatch",
                      "mismatched types for '" + selector + "': '" + typeName(receiver) + "' and '" + typeName(argumentType) + "'");
                return types_.error();
            }
            if (integerOnly && !isInteger(common)) {
                error(selectorRange, "sema.unknownMessage", "'" + selector + "' needs integer operands");
                return types_.error();
            }
            std::string op = selector.substr(selector.size() - 4, 3);
            if (kind == SendKind::minimum || kind == SendKind::maximum) {
                op = selector;
            }
            else {
                op = op == "Add" ? "+" : op == "Sub" ? "-" : "*";
            }
            addSend(send, kind, op, common, common);
            return common;
        }
    }

    for (const Node* argument : send->arguments) {
        check(argument, nullptr);
    }
    if (selector == "to:" && isInteger(receiver)) {
        error(send, "sema.intervalOutsideFor", "an interval 'a to: b' can only be the iterable of a 'for' loop for now");
        return types_.error();
    }
    error(selectorRange, "sema.unknownMessage", "'" + typeName(receiver) + "' doesn't understand '" + selector + "'");
    return types_.error();
}

// ---------------------------------------------------------------------------
// blocks

BlockInfo* Analyzer::newBlockInfo(const BlockLiteral* literal, bool inlined)
{
    auto info = std::make_unique<BlockInfo>();
    info->node = literal;
    info->inlined = inlined;
    info->parent = context_.block;
    BlockInfo* raw = info.get();
    model_.blocks[literal->id] = raw;
    model_.blockInfos.push_back(std::move(info));
    return raw;
}

// A block's value is its last statement, when that's an expression with a value.
const Type* Analyzer::checkBlockValue(const std::vector<Node*>& statements, const Type* expected, bool hasExpectation)
{
    if (statements.empty()) {
        return types_.voidType();
    }
    for (size_t i = 0; i + 1 < statements.size(); ++i) {
        checkStatement(statements[i]);
    }
    auto last = nodeCast<ExpressionStmt>(statements.back());
    if (last == nullptr) {
        checkStatement(statements.back());
        if (hasExpectation && expected != nullptr && expected != types_.voidType()) {
            error(statements.back(), "sema.missingBlockValue",
                  "this block must produce a '" + typeName(expected) + "'; end it with an expression");
        }
        return types_.voidType();
    }
    bool wantsValue = hasExpectation && expected != nullptr && expected != types_.voidType();
    const Type* type = check(last->expression, wantsValue || isValueType(expected) ? expected : nullptr);
    if (wantsValue) {
        requireAssignable(last->expression, type, expected, "produce");
        return expected;
    }
    if (hasExpectation) {
        return types_.voidType();
    }
    return isValueType(type) ? type : types_.voidType();
}

const Type* Analyzer::checkBlockLiteral(const BlockLiteral* block, const Type* expected)
{
    BlockInfo* info = newBlockInfo(block, false);
    info->symbolName = context_.message->symbolName + ".block" + std::to_string(++blockCounter_);
    if (expected != nullptr && expected->params.size() != block->params.size()) {
        error(block, "sema.blockArity",
              "this block takes " + std::to_string(block->params.size()) + " argument(s) but '" + typeName(expected) + "' takes " +
                  std::to_string(expected->params.size()));
        expected = nullptr;
    }

    FunctionContext savedContext = context_;
    auto savedScopes = scopes_.size();
    context_.block = info;
    context_.locals = &info->locals;
    context_.allowReturn = false;
    context_.loopDepth = 0;
    pushScope();

    std::vector<const Type*> params;
    bool paramsOk = true;
    for (size_t i = 0; i < block->params.size(); ++i) {
        auto param = static_cast<const BlockParam*>(block->params[i]);
        const Type* declared = param->type != nullptr ? resolveType(param->type) : nullptr;
        const Type* fromContext = expected != nullptr ? expected->params[i] : nullptr;
        const Type* type = declared != nullptr ? declared : fromContext;
        if (declared != nullptr && fromContext != nullptr && declared != fromContext && declared != types_.error()) {
            error(param, "sema.typeMismatch",
                  "block parameter '" + param->name + "' is declared '" + typeName(declared) + "' but '" + typeName(fromContext) + "' is expected");
        }
        if (type == nullptr) {
            error(param, "sema.cannotInferBlock",
                  "the type of block parameter '" + param->name + "' can't be inferred here; write it as ':" + param->name + ": int32'");
            type = types_.error();
        }
        paramsOk = paramsOk && type != types_.error();
        params.push_back(type);
        LocalInfo* local = declareLocal(param->name, type, param, param->range);
        local->isParameter = true;
        local->readOnly = true;
        info->params.push_back(local);
        model_.localRefs[param->id] = local;
    }

    const Type* result = checkBlockValue(block->statements, expected != nullptr ? expected->result : nullptr, expected != nullptr);

    popScope();
    scopes_.resize(savedScopes);
    context_ = savedContext;

    if (!paramsOk || result == types_.error()) {
        info->type = types_.error();
        return types_.error();
    }
    info->type = expected != nullptr ? expected : types_.block(std::move(params), result);
    return info->type;
}

// Fills a bare `block` parameter's shape from a concrete block type it meets.
bool Analyzer::unify(const Type* pendingType, const Type* concrete, const Node* site)
{
    Type* pending = const_cast<Type*>(pendingType);
    if (concrete->pending) {
        error(site, "sema.cannotInferBlock", "two inferred blocks meet here; annotate one with 'block takes: [T] returns: R'");
        return false;
    }
    if (pending->arityKnown && pending->params.size() != concrete->params.size()) {
        error(site, "sema.blockArity", "block arity mismatch: '" + typeName(concrete) + "'");
        return false;
    }
    if (!pending->arityKnown) {
        pending->params.assign(concrete->params.size(), nullptr);
        pending->arityKnown = true;
    }
    for (size_t i = 0; i < concrete->params.size(); ++i) {
        if (pending->params[i] == nullptr) {
            pending->params[i] = concrete->params[i];
        }
        else if (pending->params[i] != concrete->params[i]) {
            error(site, "sema.typeMismatch", "this block is used as '" + typeName(concrete) + "' elsewhere it takes '" +
                                                 typeName(pending->params[i]) + "'");
            return false;
        }
    }
    if (pending->result == nullptr) {
        pending->result = concrete->result;
    }
    else if (pending->result != concrete->result) {
        error(site, "sema.typeMismatch", "this block's result is used inconsistently");
        return false;
    }
    return true;
}

const Type* Analyzer::checkBlockInvoke(const Node* send, const Type* blockType, const std::vector<Node*>& arguments, const Type* expected)
{
    Type* pending = blockType->pending ? const_cast<Type*>(blockType) : nullptr;
    size_t arity = arguments.size();
    if (pending != nullptr && !pending->arityKnown) {
        pending->params.assign(arity, nullptr);
        pending->arityKnown = true;
    }
    if (blockType->params.size() != arity) {
        error(send, "sema.blockArity",
              "this block takes " + std::to_string(blockType->params.size()) + " argument(s), not " + std::to_string(arity));
        for (const Node* argument : arguments) {
            check(argument, nullptr);
        }
        return types_.error();
    }
    for (size_t i = 0; i < arity; ++i) {
        const Type* parameter = blockType->params[i];
        if (parameter == nullptr) {
            const Type* argumentType = check(arguments[i], nullptr);
            if (isValueType(argumentType)) {
                pending->params[i] = argumentType;
            }
            continue;
        }
        checkArgument(arguments[i], parameter);
    }

    const Type* result = blockType->result;
    if (pending != nullptr && result == nullptr && expected != nullptr && isValueType(expected)) {
        pending->result = expected;
        result = expected;
    }
    if (result == nullptr) {
        result = types_.voidType();
    }
    addSend(send, SendKind::blockInvoke, "value", blockType, result);
    return result;
}

const Type* Analyzer::checkInlinedBranch(const Node* argument, const char* selector, const Type* expected)
{
    auto literal = nodeCast<BlockLiteral>(argument);
    if (literal == nullptr || !literal->params.empty()) {
        check(argument, nullptr);
        error(argument, "sema.inlinedNeedsBlock", std::string("'") + selector + "' needs a block literal without parameters, such as '{ x := 1; }'");
        return types_.error();
    }
    newBlockInfo(literal, true);
    pushScope();
    const Type* value = checkBlockValue(literal->statements, expected, false);
    popScope();
    remember(literal, value);
    return value;
}

const Type* Analyzer::checkInlinedIf(const KeywordSend* send, const Type* expected)
{
    std::vector<const Type*> values;
    for (const Node* argument : send->arguments) {
        values.push_back(checkInlinedBranch(argument, send->selector.c_str(), expected));
    }
    const Type* result = types_.voidType();
    if (values.size() == 2 && isValueType(values[0]) && isValueType(values[1])) {
        const Type* common = commonType(values[0], values[1]);
        if (common != nullptr) {
            result = common;
        }
    }
    addSend(send, SendKind::inlineIf, send->selector, types_.boolType(), result);
    return result;
}

const Type* Analyzer::checkInlinedToDo(const KeywordSend* send)
{
    auto literal = nodeCast<BlockLiteral>(send->arguments[1]);
    if (literal == nullptr || literal->params.size() != 1) {
        check(send->arguments[0], nullptr);
        check(send->arguments[1], nullptr);
        error(send->arguments[1], "sema.inlinedNeedsBlock", "'to:do:' needs a block literal with one parameter, such as '{ :i | ... }'");
        return types_.error();
    }
    auto param = static_cast<const BlockParam*>(literal->params[0]);
    const Type* declared = param->type != nullptr ? resolveType(param->type) : nullptr;
    const Type* common = checkOperandPair(send->receiver, send->arguments[0], declared, "the bounds of 'to:do:'", send);
    const Type* elementType = common;
    if (common != types_.error() && !isInteger(common)) {
        error(send, "sema.invalidInterval", "the bounds of 'to:do:' must be integers");
        elementType = types_.error();
    }
    if (declared != nullptr && elementType != types_.error() && requireAssignable(send, elementType, declared, "iterate")) {
        elementType = declared;
    }
    BlockInfo* info = newBlockInfo(literal, true);
    pushScope();
    LocalInfo* local = declareLocal(param->name, elementType, param, param->range);
    local->readOnly = true;
    info->params.push_back(local);
    model_.localRefs[param->id] = local;
    ++context_.loopDepth;
    checkStatements(literal->statements);
    --context_.loopDepth;
    popScope();
    remember(literal, types_.voidType());
    addSend(send, SendKind::inlineToDo, "to:do:", elementType, types_.voidType());
    return types_.voidType();
}

const Type* Analyzer::checkInlinedTimesRepeat(const KeywordSend* send, const Type* receiver)
{
    auto literal = nodeCast<BlockLiteral>(send->arguments[0]);
    if (literal == nullptr || !literal->params.empty()) {
        check(send->arguments[0], nullptr);
        error(send->arguments[0], "sema.inlinedNeedsBlock", "'timesRepeat:' needs a block literal without parameters");
        return types_.error();
    }
    newBlockInfo(literal, true);
    pushScope();
    ++context_.loopDepth;
    checkStatements(literal->statements);
    --context_.loopDepth;
    popScope();
    remember(literal, types_.voidType());
    addSend(send, SendKind::inlineTimesRepeat, "timesRepeat:", receiver, types_.voidType());
    return types_.voidType();
}

// ---------------------------------------------------------------------------
// scopes and flow

LocalInfo* Analyzer::declareLocal(const std::string& name, const Type* type, const Node* decl, SourceRange nameRange)
{
    if (scopes_.back().count(name) != 0) {
        error(nameRange, "sema.redeclared", "'" + name + "' is already declared in this scope");
    }
    if (isTypeName(name) || (!model_.program.name.empty() && name == model_.program.name)) {
        error(nameRange, "sema.redeclared", "'" + name + "' is a type name and can't be used for a variable");
    }
    auto local = std::make_unique<LocalInfo>();
    local->name = name;
    local->type = type;
    local->decl = decl;
    local->owner = context_.block;
    local->index = static_cast<uint32_t>(context_.locals->size());
    LocalInfo* raw = local.get();
    context_.locals->push_back(raw);
    scopes_.back()[name] = raw;
    model_.locals.push_back(std::move(local));
    return raw;
}

// Looking up a variable owned by an enclosing function records it as captured
// by every block function between here and its owner.
LocalInfo* Analyzer::lookupLocal(const std::string& name)
{
    for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
        auto found = it->find(name);
        if (found == it->end()) {
            continue;
        }
        LocalInfo* local = found->second;
        for (BlockInfo* block = context_.block; block != nullptr && block != local->owner;
             block = const_cast<BlockInfo*>(block->parent)) {
            if (std::find(block->captures.begin(), block->captures.end(), local) == block->captures.end()) {
                block->captures.push_back(local);
            }
        }
        return local;
    }
    return nullptr;
}

const LocalInfo* Analyzer::rootLocal(const Node* node) const
{
    if (auto identifier = nodeCast<Identifier>(node)) {
        return model_.localOf(identifier);
    }
    if (auto access = nodeCast<FieldAccess>(node)) {
        return rootLocal(access->receiver);
    }
    return nullptr;
}

bool Analyzer::isTypeName(const std::string& name) const
{
    return types_.builtin(name) != nullptr || model_.recordNamed(name) != nullptr;
}

bool Analyzer::alwaysReturns(const std::vector<Node*>& statements) const
{
    for (const Node* statement : statements) {
        if (alwaysReturns(statement)) {
            return true;
        }
    }
    return false;
}

bool Analyzer::alwaysReturns(const Node* statement) const
{
    switch (statement->kind) {
    case NodeKind::returnStmt:
        return true;
    case NodeKind::statementGroup:
        return alwaysReturns(static_cast<const StatementGroup*>(statement)->statements);
    case NodeKind::ifStmt: {
        auto ifStmt = static_cast<const IfStmt*>(statement);
        return ifStmt->elseBody != nullptr && alwaysReturns(ifStmt->thenBody) && alwaysReturns(ifStmt->elseBody);
    }
    case NodeKind::loopStmt:
        return !breaksOut(static_cast<const LoopStmt*>(statement)->body, 0);
    case NodeKind::caseStmt: {
        auto caseStmt = static_cast<const CaseStmt*>(statement);
        if (caseStmt->elseBody == nullptr || !alwaysReturns(caseStmt->elseBody)) {
            return false;
        }
        for (const Node* node : caseStmt->entries) {
            auto entry = nodeCast<CaseEntry>(node);
            auto body = entry != nullptr ? nodeCast<BlockLiteral>(entry->body) : nullptr;
            if (body == nullptr || !alwaysReturns(body->statements)) {
                return false;
            }
        }
        return true;
    }
    case NodeKind::expressionStmt: {
        auto send = nodeCast<KeywordSend>(static_cast<const ExpressionStmt*>(statement)->expression);
        const SendInfo* info = send != nullptr ? model_.sendOf(send) : nullptr;
        if (info == nullptr || info->kind != SendKind::inlineIf || send->arguments.size() != 2) {
            return false;
        }
        auto first = nodeCast<BlockLiteral>(send->arguments[0]);
        auto second = nodeCast<BlockLiteral>(send->arguments[1]);
        return first != nullptr && second != nullptr && alwaysReturns(first->statements) && alwaysReturns(second->statements);
    }
    default:
        return false;
    }
}

bool Analyzer::breaksOut(const Node* node, int depth) const
{
    if (node->kind == NodeKind::breakStmt) {
        return depth == 0;
    }
    int inner = depth;
    if (node->kind == NodeKind::whileStmt || node->kind == NodeKind::forStmt || node->kind == NodeKind::loopStmt) {
        inner = depth + 1;
    }
    if (const SendInfo* send = model_.sendOf(node)) {
        if (send->kind == SendKind::inlineToDo || send->kind == SendKind::inlineTimesRepeat) {
            inner = depth + 1;
        }
    }
    for (const ChildRef& child : childrenOf(node)) {
        if (breaksOut(child.node, inner)) {
            return true;
        }
    }
    return false;
}

}

void analyze(const ast::Unit* unit, SemanticModel& model, DiagnosticEngine& diagnostics)
{
    Analyzer analyzer(model, diagnostics);
    analyzer.run(unit);
}

}
