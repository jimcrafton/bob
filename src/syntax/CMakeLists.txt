# bobSyntax: source handling, diagnostics, lexer, AST and parser.
# Deliberately has no LLVM dependency so tools (formatters, linters, language
# servers, scripting bindings) can link it on its own.
add_library(bobSyntax STATIC
    source/sourceFile.cpp
    diagnostics/diagnostics.cpp
    lexer/token.cpp
    lexer/literals.cpp
    lexer/lexer.cpp
    ast/ast.cpp
    ast/astDump.cpp
    parser/parser.cpp
)

target_include_directories(bobSyntax PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_options(bobSyntax PUBLIC /W4 /permissive- /utf-8 /Zc:__cplusplus)
