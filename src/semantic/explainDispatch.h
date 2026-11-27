#pragma once

#include "ast/ast.h"
#include "semanticModel.h"
#include "source/sourceFile.h"

#include <string>

namespace bob::sema {

// One line per send and per non-inlined block, in source order, saying how it
// is implemented: tier 0 intrinsic/inlined, tier 1 direct call, block call, or
// a block function with its captures. Backs `bobc --explain-dispatch`.
std::string explainDispatch(const SemanticModel& model, const ast::AstArena& arena, const SourceFile& file);

}
