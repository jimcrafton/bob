#include "driver/driver.h"
#include "driver/driverOptions.h"
#include "driver/toolchain.h"

#include <gtest/gtest.h>

#include <llvm/ADT/SmallString.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/Program.h>
#include <llvm/Support/raw_ostream.h>

#include <optional>
#include <string>

namespace {

struct RunResult {
    int exitCode = -1;
    std::string stdOut;
};

class EndToEnd : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_FALSE(llvm::sys::fs::createUniqueDirectory("bobTests", workDir_));
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

    int compile(const char* program, const std::string& exePath, std::string& diagnostics)
    {
        bob::driver::DriverOptions options;
        options.inputPath = std::string(BOB_TEST_PROGRAMS_DIR) + "/" + program;
        options.outputPath = exePath;
        options.runtimeDir = BOB_TEST_RUNTIME_DIR;
        llvm::raw_string_ostream out(diagnostics);
        llvm::raw_string_ostream err(diagnostics);
        bob::driver::Driver driver(out, err);
        return driver.run(options);
    }

    RunResult runExe(const std::string& exePath)
    {
        RunResult result;
        std::string outPath = workPath("stdout.txt");
        llvm::StringRef outRef(outPath);
        std::optional<llvm::StringRef> redirects[] = {std::nullopt, outRef, std::nullopt};
        llvm::StringRef args[] = {exePath};
        std::string error;
        result.exitCode = llvm::sys::ExecuteAndWait(exePath, args, std::nullopt, redirects, 30, 0, &error);
        llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> buffer = llvm::MemoryBuffer::getFile(outPath);
        if (buffer) {
            result.stdOut = std::string((*buffer)->getBuffer());
        }
        return result;
    }

    RunResult compileAndRun(const char* program)
    {
        std::string exePath = workPath("program.exe");
        std::string diagnostics;
        int compileResult = compile(program, exePath, diagnostics);
        EXPECT_EQ(compileResult, 0) << diagnostics;
        if (compileResult != 0) {
            return {};
        }
        return runExe(exePath);
    }

    llvm::SmallString<260> workDir_;
};

}

TEST_F(EndToEnd, HelloWritesToStdout)
{
    RunResult result = compileAndRun("hello.ll");
    EXPECT_EQ(result.exitCode, 0);
    EXPECT_EQ(result.stdOut, "hello from bob\n");
}

TEST_F(EndToEnd, ExitCodeIsReturnedFromMain)
{
    RunResult result = compileAndRun("exitCode.ll");
    EXPECT_EQ(result.exitCode, 42);
    EXPECT_EQ(result.stdOut, "");
}

TEST_F(EndToEnd, Utf8BytesReachPipeUnchanged)
{
    RunResult result = compileAndRun("utf8.ll");
    EXPECT_EQ(result.exitCode, 0);
    EXPECT_EQ(result.stdOut, "h\xC3\xA9llo \xE2\x9C\x93\n");
}

TEST_F(EndToEnd, LargeStackFrameUsesChkstkMemsetMemcpy)
{
    RunResult result = compileAndRun("bigFrame.ll");
    EXPECT_EQ(result.exitCode, 7);
}

TEST_F(EndToEnd, CompileOnlyProducesObject)
{
    bob::driver::DriverOptions options;
    options.inputPath = std::string(BOB_TEST_PROGRAMS_DIR) + "/hello.ll";
    options.outputPath = workPath("hello.obj");
    options.compileOnly = true;
    std::string diagnostics;
    llvm::raw_string_ostream out(diagnostics);
    llvm::raw_string_ostream err(diagnostics);
    bob::driver::Driver driver(out, err);
    EXPECT_EQ(driver.run(options), 0) << diagnostics;
    EXPECT_TRUE(llvm::sys::fs::exists(options.outputPath));
}

TEST_F(EndToEnd, InvalidIrIsReported)
{
    std::string diagnostics;
    EXPECT_NE(compile("invalid.ll", workPath("invalid.exe"), diagnostics), 0);
    EXPECT_NE(diagnostics.find("error"), std::string::npos) << diagnostics;
}

TEST_F(EndToEnd, FallsBackToSdkKernel32WhenGeneratedLibIsMissing)
{
    if (!bob::driver::Toolchain::findSdkKernel32("")) {
        GTEST_SKIP() << "no Windows SDK kernel32.lib on this machine";
    }
    std::string runtimeDir = workPath("runtimeOnly");
    ASSERT_FALSE(llvm::sys::fs::create_directory(runtimeDir));
    ASSERT_FALSE(llvm::sys::fs::copy_file(std::string(BOB_TEST_RUNTIME_DIR) + "/bobrt.lib", runtimeDir + "/bobrt.lib"));

    bob::driver::DriverOptions options;
    options.runtimeDir = runtimeDir;
    bob::driver::Toolchain toolchain(options);
    bob::driver::ToolchainPaths paths;
    ASSERT_FALSE(toolchain.resolve(paths));
    EXPECT_TRUE(paths.kernel32FromSdk);

    options.inputPath = std::string(BOB_TEST_PROGRAMS_DIR) + "/hello.ll";
    options.outputPath = workPath("sdkHello.exe");
    std::string diagnostics;
    llvm::raw_string_ostream out(diagnostics);
    llvm::raw_string_ostream err(diagnostics);
    bob::driver::Driver driver(out, err);
    ASSERT_EQ(driver.run(options), 0) << diagnostics;
    EXPECT_EQ(runExe(options.outputPath).stdOut, "hello from bob\n");
}

TEST_F(EndToEnd, SyntaxOnlyReportsAllErrors)
{
    bob::driver::DriverOptions options;
    options.inputPath = std::string(BOB_TEST_PROGRAMS_DIR) + "/syntaxError.bob";
    options.syntaxOnly = true;
    std::string output;
    std::string diagnostics;
    llvm::raw_string_ostream out(output);
    llvm::raw_string_ostream err(diagnostics);
    bob::driver::Driver driver(out, err);
    EXPECT_EQ(driver.run(options), 1);
    EXPECT_NE(diagnostics.find("syntaxError.bob:4:34: error: expected an expression"), std::string::npos) << diagnostics;
    EXPECT_NE(diagnostics.find("syntaxError.bob:7:11: error: expected ';'"), std::string::npos) << diagnostics;
    EXPECT_NE(diagnostics.find("2 errors"), std::string::npos) << diagnostics;
}

TEST_F(EndToEnd, DumpAstJson)
{
    bob::driver::DriverOptions options;
    options.inputPath = std::string(BOB_TEST_PROGRAMS_DIR) + "/syntax/designExample.bob";
    options.dumpAst = bob::driver::AstDumpFormat::json;
    std::string output;
    std::string diagnostics;
    llvm::raw_string_ostream out(output);
    llvm::raw_string_ostream err(diagnostics);
    bob::driver::Driver driver(out, err);
    EXPECT_EQ(driver.run(options), 0) << diagnostics;
    EXPECT_NE(output.find("\"kind\": \"For\""), std::string::npos);
    EXPECT_NE(output.find("\"variable\": \"i\""), std::string::npos);
}

TEST_F(EndToEnd, ProgramWithoutMainIsRejected)
{
    std::string diagnostics;
    EXPECT_NE(compile("notYet.bob", workPath("notYet.exe"), diagnostics), 0);
    EXPECT_NE(diagnostics.find("sema.noMain"), std::string::npos) << diagnostics;
}

TEST_F(EndToEnd, ExplainDispatchReportsEachSend)
{
    bob::driver::DriverOptions options;
    options.inputPath = std::string(BOB_TEST_PROGRAMS_DIR) + "/run/blocks.bob";
    options.outputPath = workPath("blocks.exe");
    options.runtimeDir = BOB_TEST_RUNTIME_DIR;
    options.explainDispatch = true;
    std::string output;
    std::string diagnostics;
    llvm::raw_string_ostream out(output);
    llvm::raw_string_ostream err(diagnostics);
    bob::driver::Driver driver(out, err);
    EXPECT_EQ(driver.run(options), 0) << diagnostics;
    EXPECT_NE(output.find("'value:' -> block call"), std::string::npos) << output;
    EXPECT_NE(output.find("'apply:to:' -> tier 1, direct call to 'blocks apply:to:'"), std::string::npos) << output;
    EXPECT_NE(output.find("'*' -> tier 0, intrinsic int32 (overflow: trap)"), std::string::npos) << output;
    EXPECT_NE(output.find("'ifTrue:ifFalse:' -> tier 0, inlined control flow"), std::string::npos) << output;
    EXPECT_NE(output.find("captures total by reference"), std::string::npos) << output;
}

TEST_F(EndToEnd, DumpLlvmPrintsTheModule)
{
    bob::driver::DriverOptions options;
    options.inputPath = std::string(BOB_TEST_PROGRAMS_DIR) + "/run/factorial.bob";
    options.dumpLlvm = true;
    std::string output;
    std::string diagnostics;
    llvm::raw_string_ostream out(output);
    llvm::raw_string_ostream err(diagnostics);
    bob::driver::Driver driver(out, err);
    EXPECT_EQ(driver.run(options), 0) << diagnostics;
    EXPECT_NE(output.find("define i32 @bobMain(ptr"), std::string::npos) << output;
    EXPECT_NE(output.find("llvm.smul.with.overflow.i32"), std::string::npos) << output;
}

TEST_F(EndToEnd, StackObjectsHaveNoRefcountCalls)
{
    bob::driver::DriverOptions options;
    options.inputPath = std::string(BOB_TEST_PROGRAMS_DIR) + "/run/stackRefcount.bob";
    options.dumpLlvm = true;
    std::string output;
    std::string diagnostics;
    llvm::raw_string_ostream out(output);
    llvm::raw_string_ostream err(diagnostics);
    bob::driver::Driver driver(out, err);
    ASSERT_EQ(driver.run(options), 0) << diagnostics;
    size_t begin = output.find("define internal i32 @\"bob.stackRefcount.main:\"");
    ASSERT_NE(begin, std::string::npos) << output;
    std::string mainBody = output.substr(begin, output.find("\n}\n", begin) - begin);
    EXPECT_EQ(mainBody.find("@bobRetain"), std::string::npos) << mainBody;
    EXPECT_EQ(mainBody.find("@bobRelease"), std::string::npos) << mainBody;
}
