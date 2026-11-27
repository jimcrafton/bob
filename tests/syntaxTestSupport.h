#pragma once

#include "ast/astDump.h"
#include "diagnostics/diagnostics.h"
#include "parser/parser.h"

#include <string>

namespace bobTest {

struct Parsed {
    std::string source;
    bob::DiagnosticEngine diagnostics;
    bob::parser::ParsedUnit unit;

    std::string diagnosticsText() const
    {
        bob::SourceFile file("test.bob", source);
        return diagnostics.format(file);
    }
};

inline std::unique_ptr<Parsed> parse(const std::string& source)
{
    auto parsed = std::make_unique<Parsed>();
    parsed->source = source;
    parsed->unit = bob::parser::parseSource(parsed->source, parsed->diagnostics);
    return parsed;
}

// Parses `{ <statements> }` and returns the text dump of the first item.
inline std::string dumpFirstItem(const std::string& statements, std::string* diagnostics = nullptr)
{
    std::unique_ptr<Parsed> parsed = parse("{\n" + statements + "\n}\n");
    if (diagnostics != nullptr) {
        *diagnostics = parsed->diagnosticsText();
    }
    if (parsed->unit.unit == nullptr || parsed->unit.unit->items.empty()) {
        return "<no items>";
    }
    return bob::ast::dumpAstText(parsed->unit.unit->items.front());
}

}
