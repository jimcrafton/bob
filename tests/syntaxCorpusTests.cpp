#include "lexer/lexer.h"
#include "syntaxTestSupport.h"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace {

std::vector<std::filesystem::path> corpusFiles()
{
    std::vector<std::filesystem::path> files;
    std::filesystem::path root = std::filesystem::path(BOB_TEST_PROGRAMS_DIR) / "syntax";
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
        if (entry.is_regular_file() && entry.path().extension() == ".bob") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    return files;
}

std::string readFile(const std::filesystem::path& path)
{
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

}

TEST(SyntaxCorpus, EveryProgramParsesWithoutDiagnostics)
{
    std::vector<std::filesystem::path> files = corpusFiles();
    ASSERT_GE(files.size(), 19u);
    for (const auto& path : files) {
        auto parsed = bobTest::parse(readFile(path));
        EXPECT_EQ(parsed->diagnostics.diagnostics().size(), 0u) << path.string() << "\n" << parsed->diagnosticsText();
        EXPECT_FALSE(parsed->unit.unit->items.empty() && parsed->unit.unit->unitKind == bob::ast::UnitKind::fragment &&
                     path.filename() != "testMsgNoReceiver.bob")
            << path.string();
    }
}

TEST(SyntaxCorpus, EveryProgramRoundTripsThroughTheTokenStream)
{
    for (const auto& path : corpusFiles()) {
        std::string source = readFile(path);
        bob::DiagnosticEngine diagnostics;
        bob::lexer::TokenStream stream = bob::lexer::tokenize(source, diagnostics);
        EXPECT_EQ(bob::lexer::reconstructSource(stream, source), source) << path.string();
    }
}
