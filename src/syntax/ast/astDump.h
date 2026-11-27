#pragma once

#include "ast/ast.h"
#include "lexer/token.h"
#include "source/sourceFile.h"

#include <string>

namespace bob::ast {

// Indented, human-readable tree: `role: Kind prop=value ...` per line.
std::string dumpAstText(const Node* root);

// Machine-readable tree for external tools. Each node has id, kind, role,
// range (offsets plus 1-based line/column when `file` is given), properties
// and children.
std::string dumpAstJson(const Node* root, const SourceFile* file);

std::string dumpTokens(const lexer::TokenStream& stream, const SourceFile& file);

}
