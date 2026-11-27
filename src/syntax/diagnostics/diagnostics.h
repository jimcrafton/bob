#pragma once

#include "source/sourceFile.h"

#include <string>
#include <vector>

namespace bob {

enum class Severity {
    error,
    warning,
    note,
};

// `code` is a stable, machine-readable id ("lex.unterminatedString", "parse.expected")
// so tools can filter diagnostics or offer fixes without matching message text.
struct Diagnostic {
    Severity severity = Severity::error;
    SourceRange range;
    std::string code;
    std::string message;
};

class DiagnosticEngine {
public:
    void report(Severity severity, SourceRange range, std::string code, std::string message);
    void error(SourceRange range, std::string code, std::string message);
    void warning(SourceRange range, std::string code, std::string message);

    const std::vector<Diagnostic>& diagnostics() const { return diagnostics_; }
    size_t errorCount() const { return errorCount_; }
    void clear();

    std::string format(const SourceFile& file) const;

private:
    std::vector<Diagnostic> diagnostics_;
    size_t errorCount_ = 0;
};

const char* severityName(Severity severity);

}
