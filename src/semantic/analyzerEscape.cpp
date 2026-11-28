// Escape analysis and stack placement of objects (design §4.3a).
//
// Each message gets a summary: may it keep `self`, or a parameter, beyond the
// call (store it in a field or a container, return it, pass it to something
// that keeps it)? Summaries start optimistic and are recomputed until nothing
// changes, so recursion converges. Within a message, variables that are
// assigned to each other form one alias group, and a group escapes if any
// member does.
//
// An object created by `x: C;` or `x := C f: …` goes on the stack when its
// group doesn't escape, every `init` in its class chain keeps `self` to
// itself, and every alias is declared in the same function and loop body as
// the allocation (so the next iteration, or the frame's exit, can't leave a
// variable pointing at reused memory).
//
// Block literals are analysed the same way: a block keeps its environment in
// the creating frame unless its value escapes, or is assigned to a variable
// declared elsewhere. A block that escapes becomes a heap closure, and every
// variable it captures escapes with it (the closure keeps them alive).

#include "analyzerInternal.h"

#include <algorithm>
#include <map>
#include <set>

namespace bob::sema::detail {

namespace {

// Objects bigger than this stay on the heap unless the declaration says @[stack].
constexpr uint64_t automaticStackLimit = 4096;

struct Escape {
    const Node* site = nullptr;
    std::string why;
};

// Where a variable is declared: the function holding it and the loops around it.
struct Scope {
    const BlockInfo* owner = nullptr;
    std::vector<int> loops;
};

struct Site {
    const Node* statement = nullptr;
    const LocalInfo* local = nullptr;
    const Node* allocation = nullptr;
    const ClassInfo* classInfo = nullptr;
    Scope scope;
    bool forced = false;
    bool heap = false;
};

// What a variable is set to, for working out which variables only ever hold
// stack objects: an allocation statement, nil, another variable, or anything else.
struct Definition {
    enum Kind { allocation, nil, copyOf, other } kind = other;
    NodeId statement = 0;
    const LocalInfo* from = nullptr;
};

// A block literal compiled to its own function.
struct BlockSite {
    const BlockInfo* info = nullptr;
    const Node* literal = nullptr;
    // The variable it's assigned to, if any.
    const LocalInfo* local = nullptr;
    // Where it's created.
    Scope scope;
    // Set when the value escapes where it's created (returned, stored, passed on).
    Escape direct;
    bool escapes = false;
    std::string why;
};

bool holdsBlock(const Type* type)
{
    type = canonical(type);
    return isBlock(type) || (isOptional(type) && isBlock(type->inner));
}

// Values whose escape matters: object references, blocks, and pointers and
// slices (which may point into a frame).
bool carriesEscape(const Type* type)
{
    type = canonical(type);
    return isReference(type) || holdsBlock(type) || isPointer(type) || isSlice(type) || (isOptional(type) && isPointer(type->inner));
}

// `x address` of a local: the pointer, and anything derived from it, must not outlive the frame.
struct PointerSite {
    const Node* send = nullptr;
    const LocalInfo* local = nullptr;
    Scope scope;
    Escape direct;
};

struct Effect {
    enum Kind { none, escape, alias } kind = none;
    const LocalInfo* target = nullptr;
    const char* why = "";
};

Effect noEffect()
{
    return {};
}

Effect escapeEffect(const char* why)
{
    return {Effect::escape, nullptr, why};
}

Effect aliasEffect(const LocalInfo* target)
{
    return {Effect::alias, target, ""};
}

const Attribute* attributeNamed(const Node* node, std::string_view name)
{
    for (const Attribute* attribute : node->attributes) {
        if (attribute->name == name) {
            return attribute;
        }
    }
    return nullptr;
}

uint64_t sizeEstimate(const Type* type)
{
    type = canonical(type);
    if (type->kind == TypeKind::record) {
        uint64_t size = 0;
        for (const FieldInfo& field : type->record->fields) {
            size += sizeEstimate(field.type);
        }
        return size;
    }
    if (type->kind == TypeKind::fixedArray) {
        return type->count * sizeEstimate(type->inner);
    }
    if (type->bits != 0 && !isOptional(type)) {
        return (type->bits + 7) / 8;
    }
    return 16;
}

class EscapeAnalysis {
public:
    EscapeAnalysis(SemanticModel& model, DiagnosticEngine& diagnostics) : model_(model), diagnostics_(diagnostics) {}

    void run()
    {
        for (const auto& message : model_.messages) {
            // Imported messages keep the summary their interface recorded.
            if (!message->imported) {
                message->paramEscapes.assign(message->params.size(), false);
            }
        }
        bool changed = true;
        for (int round = 0; changed && round < 100; ++round) {
            changed = false;
            for (const auto& message : model_.messages) {
                if (!hasBody(message.get())) {
                    continue;
                }
                analyze(message.get());
                changed = updateSummary(message.get()) || changed;
            }
        }
        for (const auto& message : model_.messages) {
            if (hasBody(message.get())) {
                analyze(message.get());
                placeObjects(message.get());
                checkPointers();
                placeBlocks();
                findStackOnlyLocals();
            }
        }
        findBoxedLocals();
    }

private:
    static bool hasBody(const MessageInfo* message)
    {
        return message->decl != nullptr && nodeCast<StatementGroup>(message->decl->body) != nullptr;
    }

    // ---- per-message state

    void analyze(MessageInfo* message)
    {
        message_ = message;
        parent_.clear();
        escapes_.clear();
        declared_.clear();
        tracked_.clear();
        sites_.clear();
        blockSites_.clear();
        pointerSites_.clear();
        definitions_.clear();
        scope_ = Scope{};
        nextLoop_ = 0;
        if (message->self != nullptr) {
            declare(message->self);
        }
        for (const LocalInfo* param : message->params) {
            declare(param);
        }
        for (const Node* statement : static_cast<const StatementGroup*>(message->decl->body)->statements) {
            walkStatement(statement);
        }
        resolveBlocks();
    }

    bool updateSummary(MessageInfo* message)
    {
        bool changed = false;
        if (message->self != nullptr && !message->selfEscapes && escaped(message->self) != nullptr) {
            message->selfEscapes = true;
            changed = true;
        }
        for (size_t i = 0; i < message->params.size(); ++i) {
            if (!message->paramEscapes[i] && escaped(message->params[i]) != nullptr) {
                message->paramEscapes[i] = true;
                changed = true;
            }
        }
        return changed;
    }

    static bool isTracked(const LocalInfo* local)
    {
        return local != nullptr && carriesEscape(local->type);
    }

    const LocalInfo* find(const LocalInfo* local)
    {
        auto it = parent_.find(local);
        if (it == parent_.end() || it->second == local) {
            return local;
        }
        const LocalInfo* root = find(it->second);
        parent_[local] = root;
        return root;
    }

    void unite(const LocalInfo* a, const LocalInfo* b)
    {
        tracked_.insert(a);
        tracked_.insert(b);
        const LocalInfo* ra = find(a);
        const LocalInfo* rb = find(b);
        if (ra == rb) {
            return;
        }
        parent_[ra] = rb;
        auto escape = escapes_.find(ra);
        if (escape != escapes_.end()) {
            escapes_.emplace(rb, escape->second);
        }
    }

    const Escape* escaped(const LocalInfo* local)
    {
        auto it = escapes_.find(find(local));
        return it == escapes_.end() ? nullptr : &it->second;
    }

    void markEscape(const LocalInfo* local, const Node* site, const char* why)
    {
        tracked_.insert(local);
        escapes_.emplace(find(local), Escape{site, why});
    }

    void declare(const LocalInfo* local)
    {
        declared_[local] = scope_;
        tracked_.insert(local);
    }

    // ---- expressions: `effect` says what happens to the value of `node`

    const LocalInfo* trackedLocal(const Node* node) const
    {
        if (node == nullptr) {
            return nullptr;
        }
        if (auto attributed = nodeCast<AttributedExpr>(node)) {
            return trackedLocal(attributed->expression);
        }
        if ((node->kind == NodeKind::identifier || node->kind == NodeKind::selfExpr) && model_.selfFieldRefs.count(node->id) == 0) {
            const LocalInfo* local = model_.localOf(node);
            return isTracked(local) ? local : nullptr;
        }
        return nullptr;
    }

    void apply(const LocalInfo* local, const Node* site, const Effect& effect)
    {
        if (effect.kind == Effect::escape) {
            markEscape(local, site, effect.why);
        }
        else if (effect.kind == Effect::alias && isTracked(effect.target)) {
            unite(local, effect.target);
        }
        else {
            tracked_.insert(local);
        }
    }

    void walkExpression(const Node* node, const Effect& effect)
    {
        if (node == nullptr) {
            return;
        }
        if (const LocalInfo* local = trackedLocal(node)) {
            apply(local, node, effect);
            return;
        }
        switch (node->kind) {
        case NodeKind::identifier:
        case NodeKind::selfExpr:
        case NodeKind::integerLiteral:
        case NodeKind::realLiteral:
        case NodeKind::stringLiteral:
        case NodeKind::charLiteral:
        case NodeKind::boolLiteral:
        case NodeKind::nilLiteral:
        case NodeKind::superExpr:
        case NodeKind::error:
            return;
        case NodeKind::attributedExpr:
            // Compile-time code runs in its own context; only its constant result is used here.
            if (model_.compileTimeNodes.count(node->id) == 0) {
                walkExpression(static_cast<const AttributedExpr*>(node)->expression, effect);
            }
            return;
        case NodeKind::fieldAccess:
            walkExpression(static_cast<const FieldAccess*>(node)->receiver, noEffect());
            return;
        case NodeKind::arrayLiteral:
            for (const Node* element : static_cast<const ArrayLiteral*>(node)->elements) {
                walkExpression(element, escapeEffect("stored in a container"));
            }
            return;
        case NodeKind::blockLiteral:
            addBlockSite(static_cast<const BlockLiteral*>(node), effect);
            walkBlockFunction(static_cast<const BlockLiteral*>(node));
            return;
        case NodeKind::unarySend:
        case NodeKind::binarySend:
        case NodeKind::keywordSend:
            walkSend(node, effect);
            return;
        default:
            for (const ChildRef& child : childrenOf(node)) {
                walkExpression(child.node, escapeEffect("used here"));
            }
            return;
        }
    }

    void addBlockSite(const BlockLiteral* literal, const Effect& effect)
    {
        const BlockInfo* info = model_.blockOf(literal);
        if (info == nullptr || info->inlined) {
            return;
        }
        BlockSite site;
        site.info = info;
        site.literal = literal;
        site.scope = scope_;
        if (effect.kind == Effect::escape) {
            site.direct = Escape{literal, effect.why};
        }
        else if (effect.kind == Effect::alias && isTracked(effect.target)) {
            site.local = effect.target;
            tracked_.insert(effect.target);
        }
        blockSites_.push_back(site);
    }

    // A block that becomes its own function: its frame is separate, and its
    // result goes to whoever invokes it.
    void walkBlockFunction(const BlockLiteral* literal)
    {
        Scope saved = scope_;
        const BlockInfo* info = model_.blockOf(literal);
        scope_ = Scope{info, {}};
        if (info != nullptr) {
            for (const LocalInfo* param : info->params) {
                declare(param);
            }
        }
        walkBody(literal->statements, escapeEffect("returned from a block"));
        scope_ = saved;
    }

    // Statements of an inlined block; the last expression's value (if the send
    // produces one) gets `valueEffect`.
    void walkBody(const std::vector<Node*>& statements, const Effect& valueEffect)
    {
        for (size_t i = 0; i < statements.size(); ++i) {
            auto last = i + 1 == statements.size() ? nodeCast<ExpressionStmt>(statements[i]) : nullptr;
            if (last != nullptr) {
                walkExpression(last->expression, valueEffect);
            }
            else {
                walkStatement(statements[i]);
            }
        }
    }

    void walkInlined(const Node* argument, const Effect& valueEffect, bool isLoop)
    {
        auto literal = nodeCast<BlockLiteral>(argument);
        if (literal == nullptr) {
            walkExpression(argument, escapeEffect("used here"));
            return;
        }
        if (isLoop) {
            scope_.loops.push_back(++nextLoop_);
        }
        if (const BlockInfo* info = model_.blockOf(literal)) {
            for (const LocalInfo* param : info->params) {
                declare(param);
            }
        }
        walkBody(literal->statements, valueEffect);
        if (isLoop) {
            scope_.loops.pop_back();
        }
    }

    std::vector<const MessageInfo*> implementations(const SendInfo& send) const
    {
        std::vector<const MessageInfo*> result;
        const MessageInfo* target = send.target;
        if (target == nullptr) {
            return result;
        }
        if (send.kind == SendKind::directMethodCall || send.kind == SendKind::superCall || send.kind == SendKind::staticCall ||
            send.kind == SendKind::classStaticCall) {
            if (!target->isRequired) {
                result.push_back(target);
            }
            return result;
        }
        for (const auto& message : model_.messages) {
            if (message->isInstance && !message->isRequired && message->owner != nullptr && message->selector == target->selector &&
                (message->owner->isKindOf(send.classInfo) || message.get() == target)) {
                result.push_back(message.get());
            }
        }
        return result;
    }

    void walkSend(const Node* node, const Effect& effect)
    {
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
        const SendInfo* send = model_.sendOf(node);
        if (send == nullptr) {
            walkExpression(receiver, escapeEffect("used here"));
            for (const Node* argument : arguments) {
                walkExpression(argument, escapeEffect("used here"));
            }
            return;
        }
        auto walkArguments = [&](const Effect& each) {
            for (const Node* argument : arguments) {
                walkExpression(argument, each);
            }
        };
        switch (send->kind) {
        case SendKind::constantValue:
        case SendKind::metaInfo:
        case SendKind::metaFields:
        case SendKind::compilerDiagnostic:
            // `x meta …` only looks at the static type; nothing is evaluated.
            walkArguments(noEffect());
            return;
        case SendKind::directMethodCall:
        case SendKind::vtableCall:
        case SendKind::dynamicSend:
        case SendKind::superCall:
        case SendKind::staticCall:
        case SendKind::classStaticCall: {
            std::vector<const MessageInfo*> targets = implementations(*send);
            bool selfKept = targets.empty();
            std::vector<bool> kept(arguments.size(), targets.empty());
            for (const MessageInfo* target : targets) {
                selfKept = selfKept || target->selfEscapes;
                for (size_t i = 0; i < arguments.size() && i < target->paramEscapes.size(); ++i) {
                    kept[i] = kept[i] || target->paramEscapes[i];
                }
            }
            Effect receiverEffect = selfKept ? escapeEffect("the receiver of a message that keeps it") : noEffect();
            if (receiver->kind == NodeKind::superExpr && message_->self != nullptr) {
                apply(message_->self, receiver, receiverEffect);
            }
            else if (send->kind != SendKind::staticCall && send->kind != SendKind::classStaticCall) {
                walkExpression(receiver, receiverEffect);
            }
            for (size_t i = 0; i < arguments.size(); ++i) {
                walkExpression(arguments[i], kept[i] ? escapeEffect("passed to a message that keeps it") : noEffect());
            }
            return;
        }
        case SendKind::fieldGet:
        case SendKind::classOf:
        case SendKind::objectIsOnStack:
        case SendKind::className:
        case SendKind::classSuperclass:
        case SendKind::classIsKindOf:
        case SendKind::classRespondsTo:
        case SendKind::classCompare:
        case SendKind::classFields:
        case SendKind::classMessages:
        case SendKind::classProtocols:
        case SendKind::classGetField:
            walkExpression(receiver, noEffect());
            walkArguments(noEffect());
            return;
        case SendKind::namedClass:
        case SendKind::classNamed:
        case SendKind::classQualifiedName:
        case SendKind::classNew:
            walkExpression(receiver, noEffect());
            walkArguments(noEffect());
            return;
        case SendKind::classSetField: {
            auto keyword = static_cast<const KeywordSend*>(node);
            walkExpression(receiver, noEffect());
            walkExpression(keyword->arguments[0], noEffect());
            walkExpression(keyword->arguments[1], noEffect());
            walkExpression(keyword->arguments[2], escapeEffect("stored into a field by reflection"));
            return;
        }
        case SendKind::classPerform: {
            // The message isn't known, so the object and arguments may be kept.
            auto keyword = static_cast<const KeywordSend*>(node);
            walkExpression(receiver, noEffect());
            walkExpression(keyword->arguments[0], noEffect());
            for (size_t i = 1; i < keyword->arguments.size(); ++i) {
                walkExpression(keyword->arguments[i], escapeEffect("passed to a message sent by reflection"));
            }
            return;
        }
        case SendKind::fieldSet:
            walkExpression(receiver, noEffect());
            walkArguments(escapeEffect("stored into a field"));
            return;
        case SendKind::referenceCompare:
        case SendKind::optionalCompare:
            walkExpression(receiver, noEffect());
            walkArguments(noEffect());
            return;
        case SendKind::constructObject:
            walkArguments(escapeEffect("stored into a field of a new object"));
            return;
        case SendKind::inlineIfNil: {
            auto keyword = static_cast<const KeywordSend*>(node);
            std::vector<std::string> parts = selectorParts(keyword->selector);
            const LocalInfo* bound = nullptr;
            for (size_t i = 0; i < parts.size(); ++i) {
                const BlockInfo* info = model_.blockOf(keyword->arguments[i]);
                if (parts[i] == "ifNotNil:" && info != nullptr && !info->params.empty()) {
                    bound = info->params[0];
                }
            }
            if (bound != nullptr) {
                declare(bound);
            }
            if (bound != nullptr) {
                define(bound, node, receiver, false);
            }
            walkExpression(receiver, bound != nullptr ? aliasEffect(bound) : noEffect());
            Effect valueEffect = carriesEscape(send->resultType) ? effect : noEffect();
            for (const Node* argument : arguments) {
                walkInlined(argument, valueEffect, false);
            }
            return;
        }
        case SendKind::inlineIf: {
            walkExpression(receiver, noEffect());
            Effect valueEffect = carriesEscape(send->resultType) ? effect : noEffect();
            for (const Node* argument : arguments) {
                walkInlined(argument, valueEffect, false);
            }
            return;
        }
        case SendKind::inlineToDo:
            walkExpression(receiver, noEffect());
            walkExpression(arguments[0], noEffect());
            walkInlined(arguments[1], noEffect(), true);
            return;
        case SendKind::inlineTimesRepeat:
        case SendKind::inlineDo:
        case SendKind::inlineEnumDo:
            walkExpression(receiver, noEffect());
            walkInlined(arguments.back(), noEffect(), true);
            return;
        case SendKind::containerAtIfAbsent:
            walkExpression(receiver, noEffect());
            walkExpression(arguments[0], escapeEffect("used as a key"));
            walkInlined(arguments[1], escapeEffect("stored in a container"), false);
            return;
        case SendKind::blockInvoke:
            walkExpression(receiver, noEffect());
            walkArguments(escapeEffect("passed to a block"));
            return;
        case SendKind::pointerAddress: {
            walkExpression(receiver, noEffect());
            if (!send->frameBound || effect.kind == Effect::none) {
                return;
            }
            PointerSite site;
            site.send = node;
            site.scope = scope_;
            const Node* root = receiver;
            while (auto access = nodeCast<FieldAccess>(root)) {
                root = access->receiver;
            }
            if (const LocalInfo* place = model_.localOf(root)) {
                auto declared = declared_.find(place);
                if (declared != declared_.end()) {
                    site.scope = declared->second;
                }
            }
            if (effect.kind == Effect::escape) {
                site.direct = Escape{node, effect.why};
            }
            else if (isTracked(effect.target)) {
                site.local = effect.target;
            }
            pointerSites_.push_back(site);
            return;
        }
        case SendKind::pointerAdvance:
        case SendKind::pointerCast:
        case SendKind::pointerSlice:
            // The result points where the receiver does.
            walkExpression(receiver, effect);
            walkArguments(noEffect());
            return;
        case SendKind::pointerLoad:
        case SendKind::pointerToInteger:
        case SendKind::pointerFromInteger:
        case SendKind::sliceCount:
        case SendKind::sliceIsEmpty:
        case SendKind::sliceAt:
        case SendKind::portFromInteger:
        case SendKind::portCast:
        case SendKind::portIn:
        case SendKind::portOut:
        case SendKind::portAdvance:
        case SendKind::portToInteger:
        case SendKind::cpuInstruction:
            walkExpression(receiver, noEffect());
            walkArguments(noEffect());
            return;
        case SendKind::pointerStore:
        case SendKind::sliceAtPut:
            walkExpression(receiver, noEffect());
            walkArguments(escapeEffect("stored through a pointer"));
            return;
        case SendKind::cFunctionInvoke:
            // As for a C import: C is trusted not to keep a pointer it's given (the call is unsafe).
            walkExpression(receiver, noEffect());
            walkArguments(noEffect());
            return;
        default:
            // Everything else (arithmetic, strings, containers ...) never takes an
            // object; anything unexpected is treated as keeping it.
            walkExpression(receiver, escapeEffect("used here"));
            walkArguments(escapeEffect("stored in a container"));
            return;
        }
    }

    // ---- statements

    const Node* constructorSend(const Node* value) const
    {
        if (auto attributed = nodeCast<AttributedExpr>(value)) {
            return constructorSend(attributed->expression);
        }
        const SendInfo* send = model_.sendOf(value);
        return send != nullptr && send->kind == SendKind::constructObject ? value : nullptr;
    }

    // A weak or unowned variable must see its object destroyed when the last
    // strong reference goes, which a stack object never is.
    static Effect valueEffect(const LocalInfo* local)
    {
        if (local != nullptr && local->ownership != Ownership::strong) {
            return escapeEffect("held by a weak or unowned reference");
        }
        return aliasEffect(local);
    }

    void addSite(const Node* statement, const LocalInfo* local, const Node* allocation, const ClassInfo* info)
    {
        Site site;
        site.statement = statement;
        site.local = local;
        site.allocation = allocation;
        site.classInfo = info;
        site.scope = scope_;
        site.forced = attributeNamed(statement, "stack") != nullptr;
        site.heap = attributeNamed(statement, "heap") != nullptr;
        sites_.push_back(site);
    }

    void walkStatement(const Node* statement)
    {
        switch (statement->kind) {
        case NodeKind::variableDecl: {
            auto decl = static_cast<const VariableDecl*>(statement);
            const LocalInfo* local = model_.localOf(decl);
            if (local != nullptr) {
                declare(local);
            }
            if (decl->initializer != nullptr) {
                walkExpression(decl->initializer, valueEffect(local));
            }
            auto object = model_.defaultObjects.find(decl->id);
            if (object != model_.defaultObjects.end()) {
                addSite(decl, local, decl, object->second);
            }
            else if (const Node* send = constructorSend(decl->initializer)) {
                addSite(decl, local, send, model_.sendOf(send)->classInfo);
            }
            define(local, decl, decl->initializer, object != model_.defaultObjects.end() || constructorSend(decl->initializer) != nullptr);
            return;
        }
        case NodeKind::assignment: {
            auto assignment = static_cast<const Assignment*>(statement);
            const LocalInfo* local = model_.localOf(assignment->target);
            if (local != nullptr && model_.selfFieldRefs.count(assignment->target->id) == 0) {
                if (model_.implicitDeclarations.count(assignment->id) != 0) {
                    declare(local);
                }
                walkExpression(assignment->value, valueEffect(local));
                if (const Node* send = constructorSend(assignment->value)) {
                    addSite(assignment, local, send, model_.sendOf(send)->classInfo);
                }
                define(local, assignment, assignment->value, constructorSend(assignment->value) != nullptr);
                return;
            }
            if (auto access = nodeCast<FieldAccess>(assignment->target)) {
                walkExpression(access->receiver, noEffect());
            }
            bool objectField = model_.selfFieldRefs.count(assignment->target->id) != 0 ||
                               (nodeCast<FieldAccess>(assignment->target) != nullptr &&
                                isClass(canonical(model_.typeOf(static_cast<const FieldAccess*>(assignment->target)->receiver))));
            walkExpression(assignment->value, objectField ? escapeEffect("stored into a field") : noEffect());
            return;
        }
        case NodeKind::expressionStmt:
            walkExpression(static_cast<const ExpressionStmt*>(statement)->expression, noEffect());
            return;
        case NodeKind::returnStmt:
            for (const Node* value : static_cast<const ReturnStmt*>(statement)->values) {
                walkExpression(value, escapeEffect("returned"));
            }
            return;
        case NodeKind::ifStmt: {
            auto ifStmt = static_cast<const IfStmt*>(statement);
            walkExpression(ifStmt->condition, noEffect());
            walkStatement(ifStmt->thenBody);
            if (ifStmt->elseBody != nullptr) {
                walkStatement(ifStmt->elseBody);
            }
            return;
        }
        case NodeKind::whileStmt: {
            auto loop = static_cast<const WhileStmt*>(statement);
            scope_.loops.push_back(++nextLoop_);
            walkExpression(loop->condition, noEffect());
            walkStatement(loop->body);
            scope_.loops.pop_back();
            return;
        }
        case NodeKind::loopStmt:
            scope_.loops.push_back(++nextLoop_);
            walkStatement(static_cast<const LoopStmt*>(statement)->body);
            scope_.loops.pop_back();
            return;
        case NodeKind::forStmt: {
            auto loop = static_cast<const ForStmt*>(statement);
            walkExpression(loop->iterable, noEffect());
            scope_.loops.push_back(++nextLoop_);
            if (const LocalInfo* local = model_.localOf(loop)) {
                declare(local);
            }
            walkStatement(loop->body);
            scope_.loops.pop_back();
            return;
        }
        case NodeKind::caseStmt: {
            auto caseStmt = static_cast<const CaseStmt*>(statement);
            walkExpression(caseStmt->subject, noEffect());
            for (const Node* node : caseStmt->entries) {
                if (auto entry = nodeCast<CaseEntry>(node)) {
                    walkInlined(entry->body, noEffect(), false);
                }
            }
            if (caseStmt->elseBody != nullptr) {
                walkStatement(caseStmt->elseBody);
            }
            return;
        }
        case NodeKind::statementGroup:
            if (model_.compileTimeNodes.count(statement->id) != 0) {
                return;
            }
            for (const Node* inner : static_cast<const StatementGroup*>(statement)->statements) {
                walkStatement(inner);
            }
            return;
        default:
            return;
        }
    }

    // ---- definitions

    void define(const LocalInfo* local, const Node* statement, const Node* value, bool allocates)
    {
        if (local == nullptr || !isReference(canonical(local->type))) {
            return;
        }
        Definition definition;
        while (auto attributed = nodeCast<AttributedExpr>(value)) {
            value = attributed->expression;
        }
        if (allocates) {
            definition.kind = Definition::allocation;
            definition.statement = statement->id;
        }
        else if (value == nullptr || value->kind == NodeKind::nilLiteral) {
            definition.kind = Definition::nil;
        }
        else if (const LocalInfo* from = trackedLocal(value)) {
            definition.kind = Definition::copyOf;
            definition.from = from;
        }
        definitions_[local].push_back(definition);
    }

    // A variable is stack-only when every definition is an allocation placed on
    // the stack, nil, or a copy of another stack-only variable. Parameters, loop
    // variables and the like have no recorded definitions, so they never are.
    void findStackOnlyLocals()
    {
        std::set<const LocalInfo*> candidates;
        for (const auto& [local, list] : definitions_) {
            candidates.insert(local);
        }
        bool changed = true;
        while (changed) {
            changed = false;
            for (auto it = candidates.begin(); it != candidates.end();) {
                bool ok = true;
                for (const Definition& definition : definitions_[*it]) {
                    switch (definition.kind) {
                    case Definition::allocation:
                        ok = ok && model_.stackObjects.count(definition.statement) != 0;
                        break;
                    case Definition::nil:
                        break;
                    case Definition::copyOf:
                        ok = ok && candidates.count(definition.from) != 0;
                        break;
                    case Definition::other:
                        ok = false;
                        break;
                    }
                }
                if (ok) {
                    ++it;
                }
                else {
                    it = candidates.erase(it);
                    changed = true;
                }
            }
        }
        model_.stackOnlyLocals.insert(candidates.begin(), candidates.end());
    }

    // ---- placement

    static bool isPrefix(const std::vector<int>& prefix, const std::vector<int>& path)
    {
        return prefix.size() <= path.size() && std::equal(prefix.begin(), prefix.end(), path.begin());
    }

    void placeObjects(const MessageInfo* message)
    {
        for (const Site& site : sites_) {
            if (site.heap) {
                continue;
            }
            const Node* where = nullptr;
            std::string why;
            if (const Escape* escape = escaped(site.local)) {
                where = escape->site;
                why = escape->why;
            }
            for (const ClassInfo* c = site.classInfo; c != nullptr && where == nullptr; c = c->superclass) {
                if (c->initMessage != nullptr && c->initMessage->selfEscapes) {
                    where = site.statement;
                    why = "'" + c->name + " init' keeps 'self'";
                }
            }
            const LocalInfo* root = find(site.local);
            for (const LocalInfo* other : tracked_) {
                if (where != nullptr || find(other) != root) {
                    continue;
                }
                auto declared = declared_.find(other);
                if (declared == declared_.end() || declared->second.owner != site.scope.owner ||
                    !isPrefix(site.scope.loops, declared->second.loops)) {
                    where = site.statement;
                    why = "it's assigned to '" + other->name + "', which outlives " +
                          (site.scope.loops.empty() ? std::string("the frame") : std::string("one pass of the loop"));
                }
            }
            uint64_t size = 16;
            for (const FieldInfo& field : site.classInfo->fields) {
                size += sizeEstimate(field.type);
            }
            if (where == nullptr && size > automaticStackLimit && !site.forced) {
                continue;
            }
            if (where != nullptr) {
                if (site.forced) {
                    diagnostics_.error(where->range, "sema.stackEscape",
                                       "'" + site.local->name + "' is @[stack], but the object escapes: " + why);
                }
                continue;
            }
            StackObject object;
            object.classInfo = site.classInfo;
            object.local = site.local;
            object.allocation = site.allocation;
            object.message = message;
            object.block = site.scope.owner;
            object.forced = site.forced;
            model_.stackObjects[site.statement->id] = object;
        }
    }

    // A frame-bound pointer is an error when it escapes, or when an alias is
    // declared in another function (a block) than the place it points at.
    void checkPointers()
    {
        for (const PointerSite& site : pointerSites_) {
            const Node* where = site.direct.site;
            std::string why = site.direct.why;
            if (where == nullptr && site.local != nullptr) {
                if (const Escape* escape = escaped(site.local)) {
                    where = escape->site;
                    why = escape->why;
                }
                const LocalInfo* root = find(site.local);
                for (const LocalInfo* other : tracked_) {
                    if (where != nullptr || find(other) != root) {
                        continue;
                    }
                    auto declared = declared_.find(other);
                    if (declared != declared_.end() && declared->second.owner != site.scope.owner) {
                        where = site.send;
                        why = "it's assigned to '" + other->name + "', which belongs to another function";
                    }
                }
            }
            if (where != nullptr) {
                diagnostics_.error(site.send->range, "sema.pointerEscapes",
                                   "this pointer is to a variable of this frame, so it can't outlive it, but it does: " + why);
            }
        }
    }

    // Whether a block outlives the frame that creates it: its value escapes, or
    // it's assigned to a variable declared in another function or outside the
    // loop that creates it.
    bool outlivesFrame(BlockSite& site)
    {
        if (site.direct.site != nullptr) {
            site.why = site.direct.why;
            return true;
        }
        if (site.local == nullptr) {
            return false;
        }
        if (const Escape* escape = escaped(site.local)) {
            site.why = escape->why;
            return true;
        }
        const LocalInfo* root = find(site.local);
        for (const LocalInfo* other : tracked_) {
            if (find(other) != root) {
                continue;
            }
            auto declared = declared_.find(other);
            if (declared == declared_.end() || declared->second.owner != site.scope.owner ||
                !isPrefix(site.scope.loops, declared->second.loops)) {
                site.why = "it's assigned to '" + other->name + "', which outlives " +
                           (site.scope.loops.empty() ? std::string("the frame") : std::string("one pass of the loop"));
                return true;
            }
        }
        return false;
    }

    // A heap closure keeps what it captures alive, so those variables escape
    // too; that can in turn make blocks held in them escape.
    void resolveBlocks()
    {
        bool changed = true;
        while (changed) {
            changed = false;
            for (BlockSite& site : blockSites_) {
                if (site.escapes || !outlivesFrame(site)) {
                    continue;
                }
                site.escapes = true;
                changed = true;
                for (const LocalInfo* captured : site.info->captures) {
                    if (isTracked(captured) && escaped(captured) == nullptr) {
                        markEscape(captured, site.literal, "captured by a block that outlives its frame");
                    }
                }
            }
        }
    }

    void placeBlocks()
    {
        for (const BlockSite& site : blockSites_) {
            if (site.escapes) {
                model_.blockEscapes[site.info] = site.why;
            }
            else {
                model_.stackBlocks.insert(site.info);
            }
        }
    }

    // Blocks that were never analysed (field initializers, compile-time code)
    // stay heap closures, so only proven stack blocks are exempt.
    void findBoxedLocals()
    {
        for (const auto& block : model_.blockInfos) {
            if (block->inlined || model_.stackBlocks.count(block.get()) != 0) {
                continue;
            }
            for (const LocalInfo* captured : block->captures) {
                if (!captured->readOnly) {
                    model_.boxedLocals.insert(captured);
                }
            }
        }
    }

    SemanticModel& model_;
    DiagnosticEngine& diagnostics_;
    MessageInfo* message_ = nullptr;
    std::map<const LocalInfo*, const LocalInfo*> parent_;
    std::map<const LocalInfo*, Escape> escapes_;
    std::map<const LocalInfo*, Scope> declared_;
    std::set<const LocalInfo*> tracked_;
    std::vector<Site> sites_;
    std::vector<BlockSite> blockSites_;
    std::vector<PointerSite> pointerSites_;
    std::map<const LocalInfo*, std::vector<Definition>> definitions_;
    Scope scope_;
    int nextLoop_ = 0;
};

}

void analyzeEscapes(SemanticModel& model, DiagnosticEngine& diagnostics)
{
    EscapeAnalysis analysis(model, diagnostics);
    analysis.run();
}

}
