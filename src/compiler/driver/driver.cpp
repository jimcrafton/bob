#include "driver/driver.h"

#include "analyzer.h"
#include "ast/astDump.h"
#include "codegen/codeGenerator.h"
#include "compiletime/evaluator.h"
#include "explainDispatch.h"
#include "driver/linker.h"
#include "driver/objectEmitter.h"
#include "driver/toolchain.h"
#include "parser/parser.h"
#include "source/sourceFile.h"

#include <llvm/ADT/SmallString.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/raw_ostream.h>

#include <functional>

namespace bob::driver {

Driver::Driver(llvm::raw_ostream& out, llvm::raw_ostream& err) : out_(out), err_(err)
{
}

int Driver::fail(const std::string& message)
{
    err_ << "bobc: error: " << message << "\n";
    return 1;
}

int Driver::run(const DriverOptions& options)
{
    if (options.showHelp) {
        out_ << driverUsage();
        return 0;
    }
    if (options.showVersion) {
        out_ << "bobc " << BOB_VERSION_STRING << "\n";
        return 0;
    }
    if (!llvm::sys::fs::exists(options.inputPath)) {
        return fail("input file '" + options.inputPath + "' not found");
    }

    llvm::StringRef input(options.inputPath);
    if (input.ends_with(".bob")) {
        return runBobSource(options);
    }
    if (!input.ends_with(".ll")) {
        return fail("unsupported input '" + options.inputPath + "' (expected a .bob or .ll file)");
    }
    if (options.dumpAst != AstDumpFormat::none || options.dumpTokens || options.syntaxOnly || options.dumpLlvm || options.explainDispatch) {
        return fail("--dump-ast, --dump-tokens, --dump-llvm, --explain-dispatch and --syntax-only apply to .bob input only");
    }
    return produceOutput(options, [&](const std::string& objectPath) { return emitObjectFromIrFile(options.inputPath, objectPath, options.optLevel); });
}

int Driver::reportDiagnostics(const DiagnosticEngine& diagnostics, const SourceFile& file)
{
    err_ << diagnostics.format(file);
    if (diagnostics.errorCount() > 0) {
        err_ << diagnostics.errorCount() << (diagnostics.errorCount() == 1 ? " error" : " errors") << "\n";
        return 1;
    }
    return 0;
}

int Driver::runBobSource(const DriverOptions& options)
{
    llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> buffer = llvm::MemoryBuffer::getFile(options.inputPath, true);
    if (!buffer) {
        return fail("cannot read '" + options.inputPath + "': " + buffer.getError().message());
    }
    SourceFile file(options.inputPath, std::string((*buffer)->getBuffer()));
    DiagnosticEngine diagnostics;
    parser::ParsedUnit parsed = parser::parseSource(file.text(), diagnostics);

    if (options.dumpTokens) {
        out_ << ast::dumpTokens(parsed.tokens, file);
    }
    if (options.dumpAst == AstDumpFormat::text) {
        out_ << ast::dumpAstText(parsed.unit);
    }
    else if (options.dumpAst == AstDumpFormat::json) {
        out_ << ast::dumpAstJson(parsed.unit, &file);
    }
    if (diagnostics.errorCount() > 0 || options.syntaxOnly || options.dumpTokens || options.dumpAst != AstDumpFormat::none) {
        return reportDiagnostics(diagnostics, file);
    }

    sema::SemanticModel model;
    sema::AnalyzeOptions analyzeOptions;
    llvm::SmallString<260> sourceDirectory(options.inputPath);
    llvm::sys::path::remove_filename(sourceDirectory);
    analyzeOptions.modulePaths.push_back(sourceDirectory.empty() ? "." : std::string(sourceDirectory));
    analyzeOptions.modulePaths.insert(analyzeOptions.modulePaths.end(), options.modulePaths.begin(), options.modulePaths.end());
    sema::parseReflectLevel(options.reflectMax, analyzeOptions.reflectMax);
    sema::analyze(parsed.unit, model, diagnostics, analyzeOptions);
    if (reportDiagnostics(diagnostics, file) != 0) {
        return 1;
    }
    if (options.explainDispatch) {
        out_ << sema::explainDispatch(model, *parsed.arena, file);
    }
    // Compile-time code (constants, @[compiletime] expressions and blocks) runs
    // now, so the code generator can embed its results.
    DiagnosticEngine compileTime;
    bool evaluated = compiletime::evaluate(parsed.unit, model, compileTime);
    if (reportDiagnostics(compileTime, file) != 0 || !evaluated) {
        return 1;
    }
    bool isModule = parsed.unit->unitKind == ast::UnitKind::module || parsed.unit->unitKind == ast::UnitKind::library;
    if (!isModule && parsed.unit->unitKind != ast::UnitKind::program && !options.compileOnly && !options.dumpLlvm) {
        return fail("only a 'program', 'module' or 'lib' unit can be built; compile fragments with -c");
    }

    std::string layoutError;
    std::string dataLayout = targetDataLayout(layoutError);
    if (dataLayout.empty()) {
        return fail(layoutError);
    }
    llvm::LLVMContext context;
    std::unique_ptr<llvm::Module> module = codegen::generateModule(parsed.unit, model, context, dataLayout);
    if (options.dumpLlvm) {
        if (std::optional<std::string> error = prepareModule(*module)) {
            return fail(*error);
        }
        module->print(out_, nullptr);
        return 0;
    }
    auto emit = [&](const std::string& objectPath) { return emitObjectFile(*module, objectPath, options.optLevel); };
    if (isModule) {
        return buildModule(options, parsed.unit, model, emit);
    }
    // A program links the code of every module it imported (directly or not).
    std::vector<std::string> libraries;
    for (const auto& imported : model.modules) {
        if (imported->libraryPath.empty()) {
            return fail("module '" + imported->name + "' has an interface (" + imported->interfacePath +
                        ") but no compiled code next to it; rebuild it with 'bobc'");
        }
        libraries.push_back(imported->libraryPath);
    }
    return produceOutput(options, emit, libraries);
}

// A module or lib: its code as a static library (or, with -c, an object file),
// and its interface file next to it (design §11.5).
int Driver::buildModule(const DriverOptions& options, const ast::Unit* unit, const sema::SemanticModel& model,
                        const std::function<std::optional<std::string>(const std::string&)>& emitObject)
{
    std::string outputPath = options.outputPath.empty() ? replaceExtension(options.inputPath, options.compileOnly ? ".obj" : ".lib") : options.outputPath;
    std::string fingerprint;
    std::string interfaceText = sema::writeInterface(unit, model, fingerprint);

    if (options.compileOnly) {
        if (std::optional<std::string> error = emitObject(outputPath)) {
            return fail(*error);
        }
    }
    else {
        llvm::SmallString<260> objectPath;
        if (std::error_code ec = llvm::sys::fs::createTemporaryFile("bobc", "obj", objectPath)) {
            return fail("cannot create a temporary object file: " + ec.message());
        }
        std::string object = std::string(objectPath);
        std::optional<std::string> error = emitObject(object);
        if (!error) {
            error = writeStaticLibrary(outputPath, object, unit->name + ".obj");
        }
        llvm::sys::fs::remove(object);
        if (error) {
            return fail(*error);
        }
    }

    std::string interfacePath = replaceExtension(outputPath, ".bobi");
    std::error_code ec;
    llvm::raw_fd_ostream file(interfacePath, ec);
    if (ec) {
        return fail("cannot write '" + interfacePath + "': " + ec.message());
    }
    file << interfaceText;
    if (options.verbose) {
        err_ << "bobc: wrote " << outputPath << " and " << interfacePath << " (interface " << fingerprint << ")\n";
    }
    return 0;
}

int Driver::produceOutput(const DriverOptions& options, const std::function<std::optional<std::string>(const std::string&)>& emitObject,
                          const std::vector<std::string>& libraries)
{
    std::string outputPath = defaultOutputPath(options);
    if (options.compileOnly) {
        if (std::optional<std::string> error = emitObject(outputPath)) {
            return fail(*error);
        }
        return 0;
    }

    Toolchain toolchain(options);
    ToolchainPaths paths;
    if (std::optional<std::string> error = toolchain.resolve(paths)) {
        return fail(*error);
    }
    if (paths.kernel32FromSdk && options.verbose) {
        err_ << "bobc: note: using Windows SDK " << paths.kernel32Library << "\n";
    }

    llvm::SmallString<260> objectPath;
    if (std::error_code ec = llvm::sys::fs::createTemporaryFile("bobc", "obj", objectPath)) {
        return fail("cannot create a temporary object file: " + ec.message());
    }
    std::string object = std::string(objectPath);

    int result = 0;
    if (std::optional<std::string> error = emitObject(object)) {
        result = fail(*error);
    }
    else {
        LinkJob job;
        job.objects.push_back(object);
        job.objects.insert(job.objects.end(), libraries.begin(), libraries.end());
        job.outputPath = outputPath;
        std::string commandLine;
        std::optional<std::string> linkError = runLink(job, paths, &commandLine);
        if (options.verbose) {
            err_ << commandLine << "\n";
        }
        if (linkError) {
            result = fail(*linkError);
        }
    }

    llvm::sys::fs::remove(object);
    return result;
}

}
