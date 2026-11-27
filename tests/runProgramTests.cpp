#include "driver/driver.h"
#include "driver/driverOptions.h"

#include <gtest/gtest.h>

#include <llvm/ADT/SmallString.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/Program.h>
#include <llvm/Support/raw_ostream.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>

// Every tests/programs/run/*.bob declares its expected outcome in header comments:
//   # expect-exit: <code>
//   # expect-stderr: <text that stderr must contain>
// Each program is compiled and run at -O0 and -O2.

namespace {

struct Expectation {
    int exitCode = 0;
    std::string stderrContains;
};

struct ProgramCase {
    std::filesystem::path path;
    int optLevel = 0;
};

std::string readText(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

Expectation readExpectation(const std::string& source)
{
    Expectation expectation;
    std::istringstream lines(source);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.rfind("# expect-exit: ", 0) == 0) {
            expectation.exitCode = std::stoi(line.substr(15));
        }
        else if (line.rfind("# expect-stderr: ", 0) == 0) {
            expectation.stderrContains = line.substr(17);
        }
    }
    return expectation;
}

std::string readFileOrEmpty(const std::string& path)
{
    llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> buffer = llvm::MemoryBuffer::getFile(path);
    return buffer ? std::string((*buffer)->getBuffer()) : std::string();
}

std::vector<ProgramCase> programCases()
{
    std::vector<ProgramCase> cases;
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::path(BOB_TEST_PROGRAMS_DIR) / "run")) {
        if (entry.path().extension() == ".bob") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    for (const auto& file : files) {
        cases.push_back({file, 0});
        cases.push_back({file, 2});
    }
    return cases;
}

class RunProgram : public ::testing::TestWithParam<ProgramCase> {
protected:
    void SetUp() override
    {
        ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("bobRun", workDir_));
    }

    void TearDown() override
    {
        llvm::sys::fs::remove_directories(workDir_);
    }

    std::string workPath(const char* name) const
    {
        llvm::SmallString<260> path(workDir_);
        llvm::sys::path::append(path, name);
        return std::string(path);
    }

    llvm::SmallString<260> workDir_;
};

}

TEST_P(RunProgram, ExitCodeAndStderrMatch)
{
    const ProgramCase& program = GetParam();
    Expectation expectation = readExpectation(readText(program.path));

    bob::driver::DriverOptions options;
    options.inputPath = program.path.string();
    options.outputPath = workPath("program.exe");
    options.runtimeDir = BOB_TEST_RUNTIME_DIR;
    options.optLevel = program.optLevel;
    std::string diagnostics;
    llvm::raw_string_ostream out(diagnostics);
    llvm::raw_string_ostream err(diagnostics);
    bob::driver::Driver driver(out, err);
    ASSERT_EQ(driver.run(options), 0) << diagnostics;

    std::string stdoutPath = workPath("stdout.txt");
    std::string stderrPath = workPath("stderr.txt");
    std::optional<llvm::StringRef> redirects[] = {std::nullopt, llvm::StringRef(stdoutPath), llvm::StringRef(stderrPath)};
    llvm::StringRef args[] = {options.outputPath};
    std::string error;
    int exitCode = llvm::sys::ExecuteAndWait(options.outputPath, args, std::nullopt, redirects, 30, 0, &error);
    std::string stderrText = readFileOrEmpty(stderrPath);

    EXPECT_EQ(exitCode, expectation.exitCode) << error << "\nstderr: " << stderrText;
    if (!expectation.stderrContains.empty()) {
        EXPECT_NE(stderrText.find(expectation.stderrContains), std::string::npos) << stderrText;
    }
}

INSTANTIATE_TEST_SUITE_P(Programs, RunProgram, ::testing::ValuesIn(programCases()), [](const auto& info) {
    std::string name = info.param.path.stem().string() + "_O" + std::to_string(info.param.optLevel);
    for (char& c : name) {
        if (!std::isalnum(static_cast<unsigned char>(c))) {
            c = '_';
        }
    }
    return name;
});
