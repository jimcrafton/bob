#pragma once

#include "diagnostics/diagnostics.h"
#include "driver/driverOptions.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace llvm {
class raw_ostream;
}

namespace bob::ast {
struct Unit;
}

namespace bob::sema {
class SemanticModel;
}

namespace bob::driver {

class Driver {
public:
    Driver(llvm::raw_ostream& out, llvm::raw_ostream& err);

    int run(const DriverOptions& options);

private:
    int fail(const std::string& message);
    int reportDiagnostics(const DiagnosticEngine& diagnostics, const SourceFile& file);
    int runBobSource(const DriverOptions& options);
    int produceOutput(const DriverOptions& options, const std::function<std::optional<std::string>(const std::string&)>& emitObject,
                      const std::vector<std::string>& libraries = {});
    int buildModule(const DriverOptions& options, const ast::Unit* unit, const sema::SemanticModel& model,
                    const std::function<std::optional<std::string>(const std::string&)>& emitObject);

    llvm::raw_ostream& out_;
    llvm::raw_ostream& err_;
};

}
