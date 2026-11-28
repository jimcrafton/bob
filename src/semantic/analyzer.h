#pragma once

#include "diagnostics/diagnostics.h"
#include "semanticModel.h"

namespace bob::sema {

// Name resolution and type checking for a parsed unit. Fills `model` and
// reports problems to `diagnostics`; it always completes, so tools get a
// partial model even for code with errors. Constructs that later phases
// implement are reported as `sema.unsupported`.
void analyze(const ast::Unit* unit, SemanticModel& model, DiagnosticEngine& diagnostics);

struct AnalyzeOptions {
    // Where `import name;` looks for `name.bobi`, in order (design §11.5).
    std::vector<std::string> modulePaths;
    // `--reflect-max=`: the most reflection metadata the unit's classes emit (design §9).
    ReflectLevel reflectMax = ReflectLevel::invoke;
};

void analyze(const ast::Unit* unit, SemanticModel& model, DiagnosticEngine& diagnostics, const AnalyzeOptions& options);

// The interface file of an analyzed (and, for constants, evaluated) module or lib:
// its `interface { }` declarations in bob syntax without bodies, inferred types
// written out, constant values as literals, escape summaries, and the
// fingerprints of the interfaces it imported. `fingerprint` receives its own.
std::string writeInterface(const ast::Unit* unit, const SemanticModel& model, std::string& fingerprint);

}
