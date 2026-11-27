#pragma once

#include "driver/driverOptions.h"

namespace llvm {
class raw_ostream;
}

namespace bob::driver {

class Driver {
public:
    Driver(llvm::raw_ostream& out, llvm::raw_ostream& err);

    int run(const DriverOptions& options);

private:
    int fail(const std::string& message);

    llvm::raw_ostream& out_;
    llvm::raw_ostream& err_;
};

}
