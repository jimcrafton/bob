#pragma once

#include "diagnostics/diagnostics.h"
#include "semanticModel.h"

namespace bob::sema {

// Name resolution and type checking for a parsed unit. Fills `model` and
// reports problems to `diagnostics`; it always completes, so tools get a
// partial model even for code with errors. Constructs that later phases
// implement are reported as `sema.unsupported`.
void analyze(const ast::Unit* unit, SemanticModel& model, DiagnosticEngine& diagnostics);

}
