# Writing tools against bobSyntax

`bobSyntax` (`src/syntax`) is the lexer, parser and AST as a standalone static library, with **no LLVM dependency**. The compiler (`bobCompiler`) is one client. Formatters, linters, syntax highlighters, language servers, documentation generators, refactoring tools and the planned Python bindings are meant to be others. Link `bobSyntax` and include from `src/syntax`.

## Design rules the library keeps
- **Lossless.** `lexer::TokenStream` keeps every byte: tokens plus trivia (whitespace, newlines, line and block comments). `lexer::reconstructSource(stream, text)` reproduces the source exactly. That's what formatters and doc-comment extraction need.
- **Never gives up.** The lexer turns bad input into `invalid` tokens. The parser turns syntax errors into diagnostics plus `Error` nodes and keeps going. You always get a complete stream and a complete tree, even for half-typed code in an editor.
- **Precise positions.** Every node has a byte `range`. Declarations also carry the range of just the name (`nameRange`, `labelRange`, `selectorRange`, `fieldRange`, `partRanges`) for go-to-definition, rename and highlighting. `SourceFile::lineColumn()` converts offsets to 1-based line and column, counting columns in code points.
- **Generic access.** Tools don't have to know every node type:
  - `ast::childrenOf(node)` gives `{role, node}` pairs in source order (`"receiver"`, `"argument"`, `"body"`…);
  - `ast::propertiesOf(node)` gives `{name, value}` string pairs (`"selector"`, `"name"`…);
  - `ast::nodeKindName(kind)` gives a stable kind name.

  Both dumps are built only from these, so they're always complete.
- **Navigable.** Nodes have `parent` links (set by `parseUnit`) and stable `id`s (`arena->byId(id)`). `ast::findInnermostNode(root, offset)` finds the node under a cursor.
- **Structured diagnostics.** Each `Diagnostic` has severity, range, message and a stable `code` (below), so tools can filter or offer fixes without matching text.
- **No global state.** Everything lives in the objects you create, so parsing several files on several threads is safe.

## Minimal use
```cpp
#include "parser/parser.h"
#include "ast/astDump.h"

std::string text = /* file contents */;          // must outlive `parsed`
bob::DiagnosticEngine diagnostics;
bob::parser::ParsedUnit parsed = bob::parser::parseSource(text, diagnostics);

for (const bob::ast::ChildRef& child : bob::ast::childrenOf(parsed.unit)) {
    // child.role == "item", child.node->kind ...
}
if (auto* cls = bob::ast::nodeCast<bob::ast::ClassDecl>(parsed.unit->items[0])) {
    // typed access when you do know the kind
}
std::string json = bob::ast::dumpAstJson(parsed.unit, nullptr);
```

## Command line (for tools written in other languages)
```
bobc --syntax-only  file.bob          # diagnostics only, exit code 1 if any error
bobc --dump-tokens  file.bob          # line:column kind "text" per token
bobc --dump-ast     file.bob          # indented text tree
bobc --dump-ast=json file.bob         # JSON tree, schema below
```
JSON node:
```json
{
  "id": 12,
  "kind": "For",
  "role": "statement",
  "range": {"begin": 210, "end": 270, "line": 13, "column": 13, "endLine": 15, "endColumn": 14},
  "properties": {"variable": "i"},
  "children": [ ... ]
}
```

## Diagnostic codes
| Code | Meaning |
|---|---|
| `lex.unexpectedCharacter` | character that can't start a token (`$`, lone `~`, `@` without `[`, non-ASCII outside strings and comments) |
| `lex.unterminatedComment` | `#{` without `}#` |
| `lex.unterminatedLiteral` | string or char literal not closed on its line |
| `lex.invalidString`, `lex.invalidChar` | bad escape, invalid UTF-8, or a char literal that isn't exactly one scalar |
| `lex.invalidNumber` | unknown suffix, overflow beyond 64 bits, malformed literal |
| `parse.expected` | generic "expected X, found Y" |
| `parse.expectedSemicolon` | missing `;` between statements (parsing continues) |
| `parse.expectedType` | a constant where a type is required |
| `parse.expectedBraces` | a control-statement body or case entry without `{ }` (braces are mandatory) |
| `parse.blockInCondition` | an unparenthesised block literal inside an `if`/`while`/`for`/`case` condition |
| `parse.invalidAssignmentTarget` | `:=` after something that isn't a variable or field |
| `parse.duplicateParameter` | two message parameters with the same name |
| `parse.messageOutsideClass` | `msg` outside a class or `extends` block |
| `parse.groupWithParameters` | statement group with block parameters |
| `parse.unexpectedOperator` | `?` used outside a type |
| `parse.trailingTokens` | tokens after the unit's closing `}` |

## Semantic model (bobSemantic)
`bobSemantic` (`src/semantic`, no LLVM) adds name resolution and types on top of the tree. `bob::sema::analyze(unit, model, diagnostics)` fills a `SemanticModel` whose results are keyed by node id (`expressionTypes`, `localRefs`, `sends` (including the dispatch tier and target of every message send), `fieldRefs`, `selfFieldRefs`, `caseRanges`, `blocks` with each block's captures). `classes` holds every class and protocol with its fields, methods, vtable layout and subclasses. `sema::explainDispatch()` renders the per-send dispatch report behind `bobc --explain-dispatch`. A language server can answer "what type is this?" or "where is this declared?" without the compiler. Details are in `docs/semantics.md`. Semantic diagnostic codes start with `sema.`: for example `sema.undeclared`, `sema.typeMismatch`, `sema.literalOutOfRange`, `sema.missingReturn`, `sema.unknownMessage`, `sema.unsupported` (a planned feature, with its phase in the message).

## Node kinds
`Unit Import Namespace Class Record Extends Enum EnumMember TypeAlias FieldGroup Field Message Param Attribute VarDecl Assign Return ExprStmt Group If While For Loop Break Continue Case CaseEntry Int Real String Char Bool Nil Self Super Ident FieldAccess Array Block BlockParam Unary Binary Keyword AttributedExpr Type Optional Tuple TupleElement TypeMessage Error`. Type positions produce `Type`, `Optional`, `Tuple`, `TypeMessage`, or constant expressions as arguments of a `TypeMessage`.
