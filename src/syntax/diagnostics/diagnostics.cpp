#include "diagnostics/diagnostics.h"

namespace bob {

const char* severityName(Severity severity)
{
    switch (severity) {
    case Severity::error:
        return "error";
    case Severity::warning:
        return "warning";
    case Severity::note:
        return "note";
    }
    return "error";
}

void DiagnosticEngine::report(Severity severity, SourceRange range, std::string code, std::string message)
{
    diagnostics_.push_back({severity, range, std::move(code), std::move(message)});
    if (severity == Severity::error) {
        ++errorCount_;
    }
}

void DiagnosticEngine::error(SourceRange range, std::string code, std::string message)
{
    report(Severity::error, range, std::move(code), std::move(message));
}

void DiagnosticEngine::warning(SourceRange range, std::string code, std::string message)
{
    report(Severity::warning, range, std::move(code), std::move(message));
}

void DiagnosticEngine::clear()
{
    diagnostics_.clear();
    errorCount_ = 0;
}

std::string DiagnosticEngine::format(const SourceFile& file) const
{
    std::string out;
    for (const Diagnostic& diagnostic : diagnostics_) {
        LineColumn begin = file.lineColumn(diagnostic.range.begin);
        out += file.path() + ":" + std::to_string(begin.line) + ":" + std::to_string(begin.column) + ": " +
               severityName(diagnostic.severity) + ": " + diagnostic.message + " [" + diagnostic.code + "]\n";

        std::string_view line = file.lineText(begin.line);
        out += "    ";
        out += line;
        out += "\n    ";
        out += std::string(begin.column - 1, ' ');
        LineColumn end = file.lineColumn(diagnostic.range.end);
        uint32_t width = 1;
        if (end.line == begin.line && end.column > begin.column) {
            width = end.column - begin.column;
        }
        out += "^";
        out += std::string(width - 1, '~');
        out += "\n";
    }
    return out;
}

}
