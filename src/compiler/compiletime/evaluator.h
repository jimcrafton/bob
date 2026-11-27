#pragma once

#include "diagnostics/diagnostics.h"
#include "semanticModel.h"

namespace bob::compiletime {

// Runs every compile-time item of an analyzed unit (unit-level constants,
// @[compiletime] expressions and blocks), each once, constants before the code
// that needs them, and stores the results in the model for the code generator
// to embed (design §8.2). Compile-time code is JIT compiled with the program's
// own code generator and runs in this process against the hosted runtime.
// Returns false (with diagnostics) when compile-time code fails or reports an
// error with `compiler error:`.
bool evaluate(const ast::Unit* unit, sema::SemanticModel& model, DiagnosticEngine& diagnostics);

}
