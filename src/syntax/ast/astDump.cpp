#include "ast/astDump.h"

#include <cstdio>

namespace bob::ast {

namespace {

std::string quoteText(const std::string& value)
{
    std::string out = "\"";
    for (unsigned char c : value) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20) {
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                out += buffer;
            }
            else {
                out += static_cast<char>(c);
            }
            break;
        }
    }
    out += "\"";
    return out;
}

bool needsQuotes(const std::string& value)
{
    if (value.empty()) {
        return true;
    }
    for (char c : value) {
        if (c == ' ' || c == '"' || c == '\\' || static_cast<unsigned char>(c) < 0x20) {
            return true;
        }
    }
    return false;
}

void dumpTextNode(std::string& out, const Node* node, const char* role, int depth)
{
    out.append(static_cast<size_t>(depth) * 2, ' ');
    if (role != nullptr) {
        out += role;
        out += ": ";
    }
    out += nodeKindName(node->kind);
    for (const Property& property : propertiesOf(node)) {
        out += ' ';
        out += property.name;
        out += '=';
        out += needsQuotes(property.value) || node->kind == NodeKind::stringLiteral ? quoteText(property.value) : property.value;
    }
    out += '\n';
    for (const ChildRef& child : childrenOf(node)) {
        dumpTextNode(out, child.node, child.role, depth + 1);
    }
}

void dumpJsonNode(std::string& out, const Node* node, const char* role, const SourceFile* file, int depth)
{
    std::string indent(static_cast<size_t>(depth) * 2, ' ');
    out += indent + "{\n";
    out += indent + "  \"id\": " + std::to_string(node->id) + ",\n";
    out += indent + "  \"kind\": " + quoteText(nodeKindName(node->kind)) + ",\n";
    if (role != nullptr) {
        out += indent + "  \"role\": " + quoteText(role) + ",\n";
    }
    out += indent + "  \"range\": {\"begin\": " + std::to_string(node->range.begin) + ", \"end\": " + std::to_string(node->range.end);
    if (file != nullptr) {
        LineColumn begin = file->lineColumn(node->range.begin);
        LineColumn end = file->lineColumn(node->range.end);
        out += ", \"line\": " + std::to_string(begin.line) + ", \"column\": " + std::to_string(begin.column) +
               ", \"endLine\": " + std::to_string(end.line) + ", \"endColumn\": " + std::to_string(end.column);
    }
    out += "},\n";
    out += indent + "  \"properties\": {";
    bool first = true;
    for (const Property& property : propertiesOf(node)) {
        out += first ? "" : ", ";
        out += quoteText(property.name) + ": " + quoteText(property.value);
        first = false;
    }
    out += "},\n";
    std::vector<ChildRef> children = childrenOf(node);
    if (children.empty()) {
        out += indent + "  \"children\": []\n";
    }
    else {
        out += indent + "  \"children\": [\n";
        for (size_t i = 0; i < children.size(); ++i) {
            dumpJsonNode(out, children[i].node, children[i].role, file, depth + 2);
            out += i + 1 < children.size() ? ",\n" : "\n";
        }
        out += indent + "  ]\n";
    }
    out += indent + "}";
}

}

std::string dumpAstText(const Node* root)
{
    std::string out;
    if (root != nullptr) {
        dumpTextNode(out, root, nullptr, 0);
    }
    return out;
}

std::string dumpAstJson(const Node* root, const SourceFile* file)
{
    std::string out;
    if (root != nullptr) {
        dumpJsonNode(out, root, nullptr, file, 0);
    }
    out += "\n";
    return out;
}

std::string dumpTokens(const lexer::TokenStream& stream, const SourceFile& file)
{
    std::string out;
    for (const lexer::Token& token : stream.tokens) {
        LineColumn position = file.lineColumn(token.range.begin);
        out += std::to_string(position.line) + ":" + std::to_string(position.column) + " " + lexer::tokenKindName(token.kind);
        if (token.kind != lexer::TokenKind::endOfFile) {
            out += " " + quoteText(std::string(token.text));
        }
        out += "\n";
    }
    return out;
}

}
