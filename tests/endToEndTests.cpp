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

TEST_F(EndToEnd, ExplainDispatchReportsItablesAndClosures)
{
    auto explain = [&](const char* program) {
        bob::driver::DriverOptions options;
        options.inputPath = std::string(BOB_TEST_PROGRAMS_DIR) + "/run/" + program + ".bob";
        options.outputPath = workPath((std::string(program) + ".exe").c_str());
        options.runtimeDir = BOB_TEST_RUNTIME_DIR;
        options.explainDispatch = true;
        std::string output;
        std::string diagnostics;
        llvm::raw_string_ostream out(output);
        llvm::raw_string_ostream err(diagnostics);
        bob::driver::Driver driver(out, err);
        EXPECT_EQ(driver.run(options), 0) << diagnostics;
        return output;
    };
    std::string itables = explain("itables");
    // Scalable refines Shape, so its slots start with Shape's four messages.
    EXPECT_NE(itables.find("'scale:' -> tier 2, itable 'Scalable' slot 4"), std::string::npos) << itables;
    EXPECT_NE(itables.find("'bounds' -> tier 2, itable 'Shape' slot 1"), std::string::npos) << itables;
    std::string closures = explain("closures");
    EXPECT_NE(closures.find("captures count (boxed) -> heap closure (returned)"), std::string::npos) << closures;
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

namespace {

// Compiles `body` (the statements of a `main:`) with `items` at unit level, and
// returns the compiler's output; `exitCode` gets the driver's result.
std::string compileSource(const std::string& directory, const std::string& items, const std::string& messages, int& exitCode)
{
    std::string source = "program probe, 1.0.0\n{\n" + items + "\nextends probe {\n" + messages +
                         "\n@[static] msg main: args:array [int32] { return 0; }\n}\n}\n";
    llvm::SmallString<260> path(directory);
    llvm::sys::path::append(path, "probe.bob");
    {
        std::error_code error;
        llvm::raw_fd_ostream file(path, error);
        file << source;
    }
    llvm::SmallString<260> exe(directory);
    llvm::sys::path::append(exe, "probe.exe");
    bob::driver::DriverOptions options;
    options.inputPath = std::string(path);
    options.outputPath = std::string(exe);
    options.runtimeDir = BOB_TEST_RUNTIME_DIR;
    std::string output;
    llvm::raw_string_ostream out(output);
    llvm::raw_string_ostream err(output);
    bob::driver::Driver driver(out, err);
    exitCode = driver.run(options);
    return output;
}

}

TEST_F(EndToEnd, CompileTimeFailuresAreCompileErrors)
{
    int exitCode = 0;
    std::string output = compileSource(workPath(""), "@[constant] BAD := self divide: 0;", "@[static] msg divide: n:int32 [int32] { return 10 / n; }", exitCode);
    EXPECT_NE(exitCode, 0);
    EXPECT_NE(output.find("couldn't run constant 'BAD': it trapped: division by zero"), std::string::npos) << output;

    output = compileSource(workPath(""), "@[compiletime] { compiler warning: \"just so you know\"; compiler error: \"stop here\"; }", "", exitCode);
    EXPECT_NE(exitCode, 0);
    EXPECT_NE(output.find("warning: just so you know"), std::string::npos) << output;
    EXPECT_NE(output.find("error: stop here [compiletime.error]"), std::string::npos) << output;

    output = compileSource(workPath(""), "@[constant] A := self viaB;\n@[constant] B := A + 1;", "@[static] msg viaB [int32] { return B; }", exitCode);
    EXPECT_NE(exitCode, 0);
    EXPECT_NE(output.find("constant"), std::string::npos) << output;

    output = compileSource(workPath(""), "@[constant] size: uint8 := 4;\n@[compiletime] { if size <> 4 { compiler error: \"no\"; } }", "", exitCode);
    EXPECT_EQ(exitCode, 0) << output;

    // Without @[constant], a unit-level declaration is an error (mutable globals come later).
    output = compileSource(workPath(""), "plain := 3;", "", exitCode);
    EXPECT_NE(exitCode, 0);
    EXPECT_NE(output.find("must be a constant: '@[constant] NAME := value;'"), std::string::npos) << output;
    EXPECT_EQ(output.find("error: no"), std::string::npos) << output;
}

namespace {

std::string readProgramFile(const char* name)
{
    llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> buffer =
        llvm::MemoryBuffer::getFile(std::string(BOB_TEST_PROGRAMS_DIR) + "/modules/" + name);
    return buffer ? std::string((*buffer)->getBuffer()) : std::string();
}

}

// Phase 7's exit criterion: a program built from a module and a static library,
// each compiled on its own, Pascal-unit style (design §11.5).
class Modules : public EndToEnd {
protected:
    void write(const std::string& name, const std::string& text)
    {
        std::error_code error;
        llvm::raw_fd_ostream file(workPath(name.c_str()), error);
        file << text;
    }

    int build(const std::string& name, std::string& output, bool compileOnly = false)
    {
        bob::driver::DriverOptions options;
        options.inputPath = workPath(name.c_str());
        options.runtimeDir = BOB_TEST_RUNTIME_DIR;
        options.compileOnly = compileOnly;
        output.clear();
        llvm::raw_string_ostream out(output);
        llvm::raw_string_ostream err(output);
        bob::driver::Driver driver(out, err);
        return driver.run(options);
    }
};

TEST_F(Modules, ProgramWithAModuleAndAStaticLibrary)
{
    for (const char* name : {"geometry.bob", "shapes.bob", "app.bob"}) {
        write(name, readProgramFile(name));
    }
    std::string output;
    ASSERT_EQ(build("geometry.bob", output), 0) << output;
    ASSERT_EQ(build("shapes.bob", output), 0) << output;
    EXPECT_TRUE(llvm::sys::fs::exists(workPath("geometry.lib")));
    EXPECT_TRUE(llvm::sys::fs::exists(workPath("shapes.bobi")));
    ASSERT_EQ(build("app.bob", output), 0) << output;
    RunResult run = runExe(workPath("app.exe"));
    EXPECT_EQ(run.exitCode, 0);

    // The interface holds declarations only: no bodies, no implementation section.
    std::string interface = readProgramFile("geometry.bob");
    llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> written = llvm::MemoryBuffer::getFile(workPath("geometry.bobi"));
    ASSERT_TRUE(static_cast<bool>(written));
    std::string bobi = std::string((*written)->getBuffer());
    EXPECT_EQ(bobi.find("Helper"), std::string::npos) << bobi;
    EXPECT_EQ(bobi.find("return "), std::string::npos) << bobi;
    EXPECT_NE(bobi.find("@[weak] target: geometry.Square?;"), std::string::npos) << bobi;
    EXPECT_NE(bobi.find("onChange: { int32 [int32] };"), std::string::npos) << bobi;
    EXPECT_NE(bobi.find("@[alias] type Scale := { int32 [int32] };"), std::string::npos) << bobi;
    EXPECT_NE(bobi.find("@[constant] SQUARES: array of: int32 := [1, 4, 9, 16, 25];"), std::string::npos) << bobi;

    // Changing only implementation code keeps the interface, so dependents stay valid.
    std::string fingerprint = bobi.substr(bobi.find("interface: \""), 30);
    std::string changed = readProgramFile("geometry.bob");
    changed.replace(changed.find("(a.x - b.x) abs"), 15, "((a.x - b.x) abs * 1)");
    write("geometry.bob", changed);
    ASSERT_EQ(build("geometry.bob", output), 0) << output;
    ASSERT_EQ(build("app.bob", output), 0) << output;
    written = llvm::MemoryBuffer::getFile(workPath("geometry.bobi"));
    EXPECT_NE(std::string((*written)->getBuffer()).find(fingerprint), std::string::npos);

    // Changing the interface makes units compiled against the old one stale.
    const std::string oldName = "@[constant] NAME := \"geometry\";";
    changed.replace(changed.find(oldName), oldName.size(), "@[constant] NAME := \"geometry2\";");
    write("geometry.bob", changed);
    ASSERT_EQ(build("geometry.bob", output), 0) << output;
    EXPECT_NE(build("app.bob", output), 0);
    EXPECT_NE(output.find("'shapes' was compiled against a different interface of 'geometry'"), std::string::npos) << output;
    ASSERT_EQ(build("shapes.bob", output), 0) << output;
    EXPECT_EQ(build("app.bob", output), 0) << output;
}

TEST_F(Modules, InterfaceRules)
{
    std::string output;
    write("a.bob", "module a, 1.0.0\n{\n    interface { record Point { x: int32; } class Base { } }\n}\n");
    write("b.bob", "module b, 2.1.0\n{\n    interface { record Point { y: int8; } }\n}\n");
    ASSERT_EQ(build("a.bob", output), 0) << output;
    ASSERT_EQ(build("b.bob", output), 0) << output;

    // Two imports declare Point: unqualified use is ambiguous; qualified works.
    write("clash.bob", "program clash, 1.0.0\nimport a;\nimport b;\n{\n    extends clash {\n        @[static] msg main: args:array [int32] {\n"
                       "            p: Point;\n            q: a.Point;\n            return 0;\n        }\n    }\n}\n");
    EXPECT_NE(build("clash.bob", output), 0);
    EXPECT_NE(output.find("'Point' is declared in both 'a' and 'b'; write 'a.Point' or 'b.Point'"), std::string::npos) << output;

    // Versions, closed classes, missing modules.
    write("rules.bob", "program rules, 1.0.0\nimport a, 1.2;\nimport b, 1.0;\nimport nowhere;\n{\n    class Mine inherits a.Base { }\n"
                       "    extends rules { @[static] msg main: args:array [int32] { return 0; } }\n}\n");
    EXPECT_NE(build("rules.bob", output), 0);
    EXPECT_NE(output.find("needs 'a' 1.2"), std::string::npos) << output;
    EXPECT_NE(output.find("needs 'b' 1.0"), std::string::npos) << output;
    EXPECT_NE(output.find("can't find module 'nowhere'"), std::string::npos) << output;
    EXPECT_NE(output.find("classes are closed to their module"), std::string::npos) << output;

    // An interface can't use an implementation type; programs have no sections.
    write("leaky.bob", "module leaky, 1.0.0\n{\n    interface { record Pair { inner: Hidden; } }\n    implementation { record Hidden { v: int32; } }\n}\n");
    EXPECT_NE(build("leaky.bob", output), 0);
    EXPECT_NE(output.find("isn't in the interface section"), std::string::npos) << output;
    write("sections.bob", "program sections, 1.0.0\n{\n    interface { record R { v: int32; } }\n"
                          "    extends sections { @[static] msg main: args:array [int32] { return 0; } }\n}\n");
    EXPECT_NE(build("sections.bob", output), 0);
    EXPECT_NE(output.find("a program's declarations are all its own"), std::string::npos) << output;
}
