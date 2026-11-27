#pragma once

#include "semanticModel.h"

#include <memory>
#include <string>

namespace llvm {
class LLVMContext;
class Module;
}

namespace bob::codegen {

// Lowers an analyzed unit (no semantic errors) to an LLVM module. The module
// defines `bobMain`, which the runtime's entry point calls. `dataLayout` is the
// target's layout string (object sizes for class infos depend on it).
std::unique_ptr<llvm::Module> generateModule(const ast::Unit* unit, const sema::SemanticModel& model, llvm::LLVMContext& context,
                                             const std::string& dataLayout);

// The module that evaluates one compile-time item: the whole program (compile-time
// messages included, no `bobMain`) plus `void thunk(ptr out)`, which runs the
// item and stores its value (as laid out in memory) at `out`. Constants it
// depends on must already be evaluated.
std::unique_ptr<llvm::Module> generateEvaluationModule(const ast::Unit* unit, const sema::SemanticModel& model, llvm::LLVMContext& context,
                                                       const std::string& dataLayout, const std::string& triple,
                                                       const sema::CompileTimeItem* item, const std::string& thunk);

// The allocation size LLVM gives a type, to check the semantic library's layout rules.
uint64_t llvmSizeOf(const sema::Type* type, const sema::SemanticModel& model, const std::string& dataLayout);

}
