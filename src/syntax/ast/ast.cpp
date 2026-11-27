#include "ast/ast.h"

#include <cstdio>

namespace bob::ast {

namespace {

void addAll(std::vector<ChildRef>& out, const char* role, const std::vector<Node*>& nodes)
{
    for (Node* node : nodes) {
        out.push_back({role, node});
    }
}

void addOne(std::vector<ChildRef>& out, const char* role, Node* node)
{
    if (node != nullptr) {
        out.push_back({role, node});
    }
}

std::string formatReal(double value)
{
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.17g", value);
    return buffer;
}

const char* unitKindName(UnitKind kind)
{
    switch (kind) {
    case UnitKind::fragment:
        return "fragment";
    case UnitKind::program:
        return "program";
    case UnitKind::library:
        return "lib";
    case UnitKind::module:
        return "module";
    }
    return "fragment";
}

const char* messageKindName(MessageKind kind)
{
    switch (kind) {
    case MessageKind::unary:
        return "unary";
    case MessageKind::binary:
        return "binary";
    case MessageKind::keyword:
        return "keyword";
    }
    return "unary";
}

}

Node* AstArena::byId(NodeId id) const
{
    if (id < firstId_ || id - firstId_ >= nodes_.size()) {
        return nullptr;
    }
    return nodes_[id - firstId_].get();
}

const char* nodeKindName(NodeKind kind)
{
    switch (kind) {
    case NodeKind::unit:
        return "Unit";
    case NodeKind::import:
        return "Import";
    case NodeKind::namespaceDecl:
        return "Namespace";
    case NodeKind::unitSection:
        return "Section";
    case NodeKind::classDecl:
        return "Class";
    case NodeKind::recordDecl:
        return "Record";
    case NodeKind::extendsDecl:
        return "Extends";
    case NodeKind::enumDecl:
        return "Enum";
    case NodeKind::enumMember:
        return "EnumMember";
    case NodeKind::typeAliasDecl:
        return "TypeAlias";
    case NodeKind::fieldGroup:
        return "FieldGroup";
    case NodeKind::fieldDecl:
        return "Field";
    case NodeKind::messageDecl:
        return "Message";
    case NodeKind::messageParam:
        return "Param";
    case NodeKind::attribute:
        return "Attribute";
    case NodeKind::variableDecl:
        return "VarDecl";
    case NodeKind::assignment:
        return "Assign";
    case NodeKind::returnStmt:
        return "Return";
    case NodeKind::expressionStmt:
        return "ExprStmt";
    case NodeKind::statementGroup:
        return "Group";
    case NodeKind::ifStmt:
        return "If";
    case NodeKind::whileStmt:
        return "While";
    case NodeKind::forStmt:
        return "For";
    case NodeKind::loopStmt:
        return "Loop";
    case NodeKind::breakStmt:
        return "Break";
    case NodeKind::continueStmt:
        return "Continue";
    case NodeKind::caseStmt:
        return "Case";
    case NodeKind::integerLiteral:
        return "Int";
    case NodeKind::realLiteral:
        return "Real";
    case NodeKind::stringLiteral:
        return "String";
    case NodeKind::charLiteral:
        return "Char";
    case NodeKind::boolLiteral:
        return "Bool";
    case NodeKind::nilLiteral:
        return "Nil";
    case NodeKind::selfExpr:
        return "Self";
    case NodeKind::superExpr:
        return "Super";
    case NodeKind::identifier:
        return "Ident";
    case NodeKind::fieldAccess:
        return "FieldAccess";
    case NodeKind::arrayLiteral:
        return "Array";
    case NodeKind::blockLiteral:
        return "Block";
    case NodeKind::blockParam:
        return "BlockParam";
    case NodeKind::unarySend:
        return "Unary";
    case NodeKind::binarySend:
        return "Binary";
    case NodeKind::keywordSend:
        return "Keyword";
    case NodeKind::caseEntry:
        return "CaseEntry";
    case NodeKind::attributedExpr:
        return "AttributedExpr";
    case NodeKind::typeName:
        return "Type";
    case NodeKind::optionalType:
        return "Optional";
    case NodeKind::tupleType:
        return "Tuple";
    case NodeKind::tupleTypeElement:
        return "TupleElement";
    case NodeKind::typeMessage:
        return "TypeMessage";
    case NodeKind::error:
        return "Error";
    }
    return "Unknown";
}

std::vector<ChildRef> childrenOf(const Node* node)
{
    std::vector<ChildRef> out;
    if (node == nullptr) {
        return out;
    }
    for (Attribute* attribute : node->attributes) {
        out.push_back({"attribute", attribute});
    }
    switch (node->kind) {
    case NodeKind::unit: {
        auto n = static_cast<const Unit*>(node);
        for (Import* import : n->imports) {
            out.push_back({"import", import});
        }
        addAll(out, "item", n->items);
        break;
    }
    case NodeKind::namespaceDecl:
        addAll(out, "item", static_cast<const NamespaceDecl*>(node)->items);
        break;
    case NodeKind::unitSection:
        addAll(out, "item", static_cast<const UnitSection*>(node)->items);
        break;
    case NodeKind::classDecl: {
        auto n = static_cast<const ClassDecl*>(node);
        addOne(out, "inherits", n->superclass);
        addAll(out, "implements", n->protocols);
        addAll(out, "member", n->members);
        break;
    }
    case NodeKind::recordDecl: {
        auto n = static_cast<const RecordDecl*>(node);
        addOne(out, "inherits", n->base);
        addAll(out, "field", n->fields);
        break;
    }
    case NodeKind::extendsDecl: {
        auto n = static_cast<const ExtendsDecl*>(node);
        addOne(out, "target", n->target);
        addAll(out, "implements", n->protocols);
        addAll(out, "member", n->members);
        break;
    }
    case NodeKind::enumDecl: {
        auto n = static_cast<const EnumDecl*>(node);
        addOne(out, "storage", n->storageType);
        addAll(out, "member", n->members);
        break;
    }
    case NodeKind::enumMember:
        addOne(out, "value", static_cast<const EnumMember*>(node)->value);
        break;
    case NodeKind::typeAliasDecl: {
        auto n = static_cast<const TypeAliasDecl*>(node);
        addOne(out, "storage", n->storageType);
        addOne(out, "type", n->type);
        break;
    }
    case NodeKind::fieldGroup:
        addAll(out, "field", static_cast<const FieldGroup*>(node)->fields);
        break;
    case NodeKind::fieldDecl: {
        auto n = static_cast<const FieldDecl*>(node);
        addOne(out, "type", n->type);
        addOne(out, "init", n->initializer);
        break;
    }
    case NodeKind::messageDecl: {
        auto n = static_cast<const MessageDecl*>(node);
        addAll(out, "param", n->params);
        addOne(out, "returns", n->returnType);
        addOne(out, "body", n->body);
        break;
    }
    case NodeKind::messageParam:
        addOne(out, "type", static_cast<const MessageParam*>(node)->type);
        break;
    case NodeKind::attribute:
        addOne(out, "value", static_cast<const Attribute*>(node)->value);
        break;
    case NodeKind::variableDecl: {
        auto n = static_cast<const VariableDecl*>(node);
        addOne(out, "type", n->type);
        addOne(out, "init", n->initializer);
        break;
    }
    case NodeKind::assignment: {
        auto n = static_cast<const Assignment*>(node);
        addOne(out, "target", n->target);
        addOne(out, "value", n->value);
        break;
    }
    case NodeKind::returnStmt:
        addAll(out, "value", static_cast<const ReturnStmt*>(node)->values);
        break;
    case NodeKind::expressionStmt:
        addOne(out, "expression", static_cast<const ExpressionStmt*>(node)->expression);
        break;
    case NodeKind::statementGroup:
        addAll(out, "statement", static_cast<const StatementGroup*>(node)->statements);
        break;
    case NodeKind::ifStmt: {
        auto n = static_cast<const IfStmt*>(node);
        addOne(out, "condition", n->condition);
        addOne(out, "then", n->thenBody);
        addOne(out, "else", n->elseBody);
        break;
    }
    case NodeKind::whileStmt: {
        auto n = static_cast<const WhileStmt*>(node);
        addOne(out, "condition", n->condition);
        addOne(out, "body", n->body);
        break;
    }
    case NodeKind::forStmt: {
        auto n = static_cast<const ForStmt*>(node);
        addOne(out, "type", n->variableType);
        addOne(out, "iterable", n->iterable);
        addOne(out, "body", n->body);
        break;
    }
    case NodeKind::loopStmt:
        addOne(out, "body", static_cast<const LoopStmt*>(node)->body);
        break;
    case NodeKind::caseStmt: {
        auto n = static_cast<const CaseStmt*>(node);
        addOne(out, "subject", n->subject);
        addAll(out, "entry", n->entries);
        addOne(out, "else", n->elseBody);
        break;
    }
    case NodeKind::fieldAccess:
        addOne(out, "receiver", static_cast<const FieldAccess*>(node)->receiver);
        break;
    case NodeKind::arrayLiteral:
        addAll(out, "element", static_cast<const ArrayLiteral*>(node)->elements);
        break;
    case NodeKind::blockParam:
        addOne(out, "type", static_cast<const BlockParam*>(node)->type);
        break;
    case NodeKind::blockLiteral: {
        auto n = static_cast<const BlockLiteral*>(node);
        addAll(out, "param", n->params);
        addAll(out, "statement", n->statements);
        break;
    }
    case NodeKind::unarySend:
        addOne(out, "receiver", static_cast<const UnarySend*>(node)->receiver);
        break;
    case NodeKind::binarySend: {
        auto n = static_cast<const BinarySend*>(node);
        addOne(out, "receiver", n->receiver);
        addOne(out, "argument", n->argument);
        break;
    }
    case NodeKind::keywordSend: {
        auto n = static_cast<const KeywordSend*>(node);
        addOne(out, "receiver", n->receiver);
        addAll(out, "argument", n->arguments);
        break;
    }
    case NodeKind::caseEntry: {
        auto n = static_cast<const CaseEntry*>(node);
        addAll(out, "label", n->labels);
        addOne(out, "body", n->body);
        break;
    }
    case NodeKind::attributedExpr:
        addOne(out, "expression", static_cast<const AttributedExpr*>(node)->expression);
        break;
    case NodeKind::optionalType:
        addOne(out, "inner", static_cast<const OptionalType*>(node)->inner);
        break;
    case NodeKind::tupleType:
        addAll(out, "element", static_cast<const TupleType*>(node)->elements);
        break;
    case NodeKind::tupleTypeElement:
        addOne(out, "type", static_cast<const TupleTypeElement*>(node)->type);
        break;
    case NodeKind::typeMessage: {
        auto n = static_cast<const TypeMessage*>(node);
        addOne(out, "receiver", n->receiver);
        addAll(out, "argument", n->arguments);
        break;
    }
    case NodeKind::import:
    case NodeKind::integerLiteral:
    case NodeKind::realLiteral:
    case NodeKind::stringLiteral:
    case NodeKind::charLiteral:
    case NodeKind::boolLiteral:
    case NodeKind::nilLiteral:
    case NodeKind::selfExpr:
    case NodeKind::superExpr:
    case NodeKind::identifier:
    case NodeKind::typeName:
    case NodeKind::breakStmt:
    case NodeKind::continueStmt:
    case NodeKind::error:
        break;
    }
    return out;
}

std::vector<Property> propertiesOf(const Node* node)
{
    std::vector<Property> out;
    if (node == nullptr) {
        return out;
    }
    switch (node->kind) {
    case NodeKind::unit: {
        auto n = static_cast<const Unit*>(node);
        out.push_back({"kind", unitKindName(n->unitKind)});
        if (!n->name.empty()) {
            out.push_back({"name", n->name});
        }
        if (!n->version.empty()) {
            out.push_back({"version", n->version});
        }
        break;
    }
    case NodeKind::import: {
        auto n = static_cast<const Import*>(node);
        out.push_back({"name", n->name});
        if (!n->version.empty()) {
            out.push_back({"version", n->version});
        }
        break;
    }
    case NodeKind::namespaceDecl:
        out.push_back({"name", static_cast<const NamespaceDecl*>(node)->name});
        break;
    case NodeKind::unitSection:
        out.push_back({"kind", static_cast<const UnitSection*>(node)->isInterface ? "interface" : "implementation"});
        break;
    case NodeKind::classDecl:
        out.push_back({"name", static_cast<const ClassDecl*>(node)->name});
        break;
    case NodeKind::recordDecl:
        out.push_back({"name", static_cast<const RecordDecl*>(node)->name});
        break;
    case NodeKind::enumDecl:
        out.push_back({"name", static_cast<const EnumDecl*>(node)->name});
        break;
    case NodeKind::enumMember:
        out.push_back({"name", static_cast<const EnumMember*>(node)->name});
        break;
    case NodeKind::typeAliasDecl:
        out.push_back({"name", static_cast<const TypeAliasDecl*>(node)->name});
        break;
    case NodeKind::fieldGroup:
        out.push_back({"visibility", static_cast<const FieldGroup*>(node)->visibility == Visibility::publicScope ? "public" : "private"});
        break;
    case NodeKind::fieldDecl:
        out.push_back({"name", static_cast<const FieldDecl*>(node)->name});
        break;
    case NodeKind::messageDecl: {
        auto n = static_cast<const MessageDecl*>(node);
        out.push_back({"selector", n->selector});
        out.push_back({"kind", messageKindName(n->messageKind)});
        if (n->body == nullptr) {
            out.push_back({"required", "true"});
        }
        break;
    }
    case NodeKind::messageParam: {
        auto n = static_cast<const MessageParam*>(node);
        if (!n->label.empty() && n->label != n->name) {
            out.push_back({"label", n->label});
        }
        out.push_back({"name", n->name});
        break;
    }
    case NodeKind::attribute:
        out.push_back({"name", static_cast<const Attribute*>(node)->name});
        break;
    case NodeKind::variableDecl:
        out.push_back({"name", static_cast<const VariableDecl*>(node)->name});
        break;
    case NodeKind::integerLiteral: {
        auto n = static_cast<const IntegerLiteral*>(node);
        out.push_back({"value", (n->negative ? "-" : "") + std::to_string(n->value)});
        if (!n->suffix.empty()) {
            out.push_back({"suffix", n->suffix});
        }
        break;
    }
    case NodeKind::realLiteral: {
        auto n = static_cast<const RealLiteral*>(node);
        out.push_back({"value", formatReal(n->value)});
        if (!n->suffix.empty()) {
            out.push_back({"suffix", n->suffix});
        }
        break;
    }
    case NodeKind::stringLiteral:
        out.push_back({"value", static_cast<const StringLiteral*>(node)->value});
        break;
    case NodeKind::charLiteral: {
        char buffer[16];
        std::snprintf(buffer, sizeof(buffer), "U+%04X", static_cast<const CharLiteral*>(node)->codepoint);
        out.push_back({"codepoint", buffer});
        break;
    }
    case NodeKind::boolLiteral:
        out.push_back({"value", static_cast<const BoolLiteral*>(node)->value ? "true" : "false"});
        break;
    case NodeKind::identifier:
        out.push_back({"name", static_cast<const Identifier*>(node)->name});
        break;
    case NodeKind::fieldAccess:
        out.push_back({"field", static_cast<const FieldAccess*>(node)->field});
        break;
    case NodeKind::blockParam:
        out.push_back({"name", static_cast<const BlockParam*>(node)->name});
        break;
    case NodeKind::unarySend:
        out.push_back({"selector", static_cast<const UnarySend*>(node)->selector});
        break;
    case NodeKind::binarySend:
        out.push_back({"selector", static_cast<const BinarySend*>(node)->selector});
        break;
    case NodeKind::keywordSend:
        out.push_back({"selector", static_cast<const KeywordSend*>(node)->selector});
        break;
    case NodeKind::typeName:
        out.push_back({"name", static_cast<const TypeName*>(node)->name});
        break;
    case NodeKind::tupleTypeElement: {
        auto n = static_cast<const TupleTypeElement*>(node);
        if (!n->name.empty()) {
            out.push_back({"name", n->name});
        }
        break;
    }
    case NodeKind::typeMessage:
        out.push_back({"selector", static_cast<const TypeMessage*>(node)->selector});
        break;
    case NodeKind::forStmt:
        out.push_back({"variable", static_cast<const ForStmt*>(node)->variable});
        break;
    case NodeKind::ifStmt:
    case NodeKind::whileStmt:
    case NodeKind::loopStmt:
    case NodeKind::breakStmt:
    case NodeKind::continueStmt:
    case NodeKind::caseStmt:
    case NodeKind::extendsDecl:
    case NodeKind::assignment:
    case NodeKind::returnStmt:
    case NodeKind::expressionStmt:
    case NodeKind::statementGroup:
    case NodeKind::nilLiteral:
    case NodeKind::selfExpr:
    case NodeKind::superExpr:
    case NodeKind::arrayLiteral:
    case NodeKind::blockLiteral:
    case NodeKind::caseEntry:
    case NodeKind::attributedExpr:
    case NodeKind::optionalType:
    case NodeKind::tupleType:
    case NodeKind::error:
        break;
    }
    return out;
}

void linkParents(Node* root)
{
    if (root == nullptr) {
        return;
    }
    std::vector<Node*> pending{root};
    while (!pending.empty()) {
        Node* node = pending.back();
        pending.pop_back();
        for (const ChildRef& child : childrenOf(node)) {
            child.node->parent = node;
            pending.push_back(child.node);
        }
    }
}

Node* findInnermostNode(Node* root, uint32_t offset)
{
    if (root == nullptr || offset < root->range.begin || offset >= root->range.end) {
        return nullptr;
    }
    Node* best = root;
    bool descended = true;
    while (descended) {
        descended = false;
        for (const ChildRef& child : childrenOf(best)) {
            if (offset >= child.node->range.begin && offset < child.node->range.end) {
                best = child.node;
                descended = true;
                break;
            }
        }
    }
    return best;
}

bool isTypeNode(const Node* node)
{
    if (node == nullptr) {
        return false;
    }
    switch (node->kind) {
    case NodeKind::typeName:
    case NodeKind::optionalType:
    case NodeKind::tupleType:
    case NodeKind::typeMessage:
        return true;
    default:
        return false;
    }
}

}
