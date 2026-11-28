#include "driver/driverOptions.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using bob::driver::DriverOptions;
using bob::driver::defaultOutputPath;
using bob::driver::parseDriverArguments;

namespace {

std::optional<std::string> parse(std::vector<std::string> args, DriverOptions& options)
{
    return parseDriverArguments(args, options);
}

}

TEST(DriverOptions, ParsesInputAndOutput)
{
    DriverOptions options;
    EXPECT_FALSE(parse({"hello.ll", "-o", "out/hello.exe"}, options));
    EXPECT_EQ(options.inputPath, "hello.ll");
    EXPECT_EQ(options.outputPath, "out/hello.exe");
    EXPECT_FALSE(options.compileOnly);
}

TEST(DriverOptions, ObjectsAndLibrariesAreLinkInputs)
{
    DriverOptions options;
    EXPECT_FALSE(parse({"switch.OBJ", "kernel.bob", "user32.lib"}, options));
    EXPECT_EQ(options.inputPath, "kernel.bob");
    EXPECT_EQ(options.linkInputs, (std::vector<std::string>{"switch.OBJ", "user32.lib"}));

    DriverOptions compileOnly;
    std::optional<std::string> error = parse({"-c", "kernel.bob", "switch.obj"}, compileOnly);
    ASSERT_TRUE(error);
    EXPECT_NE(error->find("-c doesn't link"), std::string::npos) << *error;

    DriverOptions alone;
    error = parse({"switch.obj"}, alone);
    ASSERT_TRUE(error);
    EXPECT_NE(error->find("no .bob or .ll input"), std::string::npos) << *error;
}

TEST(DriverOptions, ParsesValueOptions)
{
    DriverOptions options;
    EXPECT_FALSE(parse({"--linker=C:/llvm/lld-link.exe", "--runtime-dir=D:/rt", "--sdk-lib-path=D:/sdk", "-c", "-v", "a.ll"}, options));
    EXPECT_EQ(options.linkerPath, "C:/llvm/lld-link.exe");
    EXPECT_EQ(options.runtimeDir, "D:/rt");
    EXPECT_EQ(options.sdkLibPath, "D:/sdk");
    EXPECT_TRUE(options.compileOnly);
    EXPECT_TRUE(options.verbose);
}

TEST(DriverOptions, RejectsUnknownOption)
{
    DriverOptions options;
    std::optional<std::string> error = parse({"--frobnicate", "a.ll"}, options);
    ASSERT_TRUE(error);
    EXPECT_NE(error->find("--frobnicate"), std::string::npos);
}

TEST(DriverOptions, RequiresInput)
{
    DriverOptions options;
    EXPECT_TRUE(parse({"-v"}, options));
}

TEST(DriverOptions, RejectsSecondInput)
{
    DriverOptions options;
    EXPECT_TRUE(parse({"a.ll", "b.ll"}, options));
}

TEST(DriverOptions, MissingOutputValue)
{
    DriverOptions options;
    EXPECT_TRUE(parse({"a.ll", "-o"}, options));
}

TEST(DriverOptions, HelpNeedsNoInput)
{
    DriverOptions options;
    EXPECT_FALSE(parse({"--help"}, options));
    EXPECT_TRUE(options.showHelp);
}

TEST(DriverOptions, DefaultOutputPath)
{
    DriverOptions options;
    options.inputPath = "dir.v2/prog.ll";
    EXPECT_EQ(defaultOutputPath(options), "dir.v2/prog.exe");
    options.compileOnly = true;
    EXPECT_EQ(defaultOutputPath(options), "dir.v2/prog.obj");
    options.inputPath = "dir.v2/noext";
    EXPECT_EQ(defaultOutputPath(options), "dir.v2/noext.obj");
    options.outputPath = "x.obj";
    EXPECT_EQ(defaultOutputPath(options), "x.obj");
}
