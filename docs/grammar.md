# bob grammar (as implemented)

This is the grammar the `bobSyntax` parser accepts (phase 1). `docs/design.md` §3–§4 says *why*; this file says *exactly what parses*. Notation: `[x]` optional, `{x}` zero or more, `|` alternatives, `'x'` literal token.

## Lexical structure

| Token | Form | Notes |
|---|---|---|
| identifier | `[A-Za-z_][A-Za-z0-9_]*` | ASCII only |
| keywordSelector | identifier immediately followed by `:` (not `:=`) | `at:` is one token. `at :` is identifier + colon |
| integer | `123`, `1_000_000`, `0x7F`, `0b1010`, optional suffix `i8…i128`, `u8…u128` | `10u8` |
| real | `3.25`, `1e-9`, `2.5e+3`, optional suffix `r32`/`r64` | `3.0r32`. An integer with an `r` suffix is a real (`2r64`) |
| version | three or more dot-separated integers: `1.4.11` | only meaningful in unit headers and imports |
| string | `"…"` | escapes `\n \t \r \0 \\ \" \' \u{1F600}`. Must be valid UTF-8, no raw newlines |
| char | `'…'` | exactly one Unicode scalar value after escapes |
| binaryOperator | `<< >> <= >= == ~= ??` or one of `+ - * / % & \| ^ < > = ?` | maximal munch over that set |
| punctuation | `:=` `:` `;` `,` `.` `(` `)` `{` `}` `[` `]` `@[` | |
| comments | `# …` to end of line, `#{ … }#` block (not nested) | trivia |

**Reserved words** (never identifiers): `return self super nil true false and or if else while for in loop break continue case`.

**Contextual words** are ordinary identifiers that only mean something at the start of a declaration: `program lib module import namespace class record enum type extends inherits implements public private msg`. So `obj class` is a unary send, and `msg` or `type` can be variable names.

The token stream is **lossless**: whitespace, newlines and comments are trivia attached to the following token (file-trailing trivia belongs to `endOfFile`), and `reconstructSource()` reproduces the input byte for byte, including a UTF-8 BOM. Bad input produces an `invalid` token plus a diagnostic, and lexing continues.

## Units
```
unit        = [attributes] [header] '{' {item} '}'
header      = ('program' | 'lib' | 'module') qualifiedName [',' version] {import}
import      = 'import' qualifiedName [',' version] ';'
version     = versionLiteral | realLiteral | integerLiteral
qualifiedName = identifier {'.' identifier}
```
A unit without a header is a *fragment* (`{ … }`), as in scratch-lang's tests.

## Items (unit and namespace level)
```
item        = [attributes] ( namespace | class | record | enum | typeAlias | extends | statement )
namespace   = 'namespace' qualifiedName '{' {item} '}'
class       = 'class' identifier ['inherits' typeName] ['implements' typeName {',' typeName}]
              '{' {classMember} '}'
classMember = [attributes] ( ('public' | 'private') '{' {field} '}' | message )
record      = 'record' identifier ['inherits' typeName] '{' {field} '}'
extends     = 'extends' typeName ['implements' typeName {',' typeName}] '{' {[attributes] message} '}'
enum        = 'enum' declName '{' {identifier [':=' expression] ';'} '}'
typeAlias   = 'type' declName ':=' type ';'
declName    = identifier [':' type] | keywordSelector type      # `PciClass : uint8` or `PciClass: uint8`
field       = [attributes] keywordPart type [':=' expression] ';'
keywordPart = keywordSelector | identifier ':'
```
`msg` at unit or namespace level is parsed (for recovery) and reported as `parse.messageOutsideClass`.

## Messages
```
message     = 'msg' pattern ['[' typeList ']'] ( body | ';' )      # ';' = required (protocol) message
pattern     = identifier                                          # unary:   msg increment
            | binaryOperator keywordPart paramType                # binary:  msg + other:Point
            | keywordPart keywordPart paramType                   # keyword: msg at: i:int64
              { keywordPart [keywordPart] paramType }             #          put: value:int32  (rename) | isFat:bool (compact)
paramType   = typeTerm                                            # no unbracketed type messages, see below
body        = '{' statementList '}'
```
The selector is every label plus `:` (`toString:isFat:`). For later parameters, a second keyword part means "rename" (`put: value:T` gives label `put`, name `value`). Otherwise the label is also the name. Duplicate parameter names are an error.

**Parameter types must be simple or bracketed.** `msg sum: nums:[array of: int32]`. Unbracketed, `int32 isFat:` would read as a type message.

## Statements
```
statementList = { statement }                 # ';' required between statements, optional before '}'
statement   = [attributes] ( 'return' [expression {',' expression}] ';'
                           | ifStatement | whileStatement | forStatement | loopStatement | caseStatement
                           | ('break' | 'continue') ';'
                           | keywordPart type [':=' expression] ';'         # explicit declaration
                           | blockStatement
                           | expression [':=' expression] ';' )             # assignment target: identifier or field access
blockStatement = block                        # statement group, when nothing follows on the same line
               | block messageContinuation    # block receiver: `{ … } fork;`

ifStatement    = 'if' condition body ['else' (ifStatement | body)] [';']
whileStatement = 'while' condition body [';']
forStatement   = 'for' (identifier [':' typeTerm] | keywordSelector typeTerm) 'in' condition body [';']
loopStatement  = 'loop' body [';']
caseStatement  = 'case' condition 'of' '{' { caseEntry } '}' ['else' body] [';']
caseEntry      = labels ':' block [';']                                     # block may bind `{ :n | … }` for unions
labels         = binaryExpr { ',' binaryExpr }
condition      = expression                   # no unparenthesised block literal anywhere inside
body           = '{' statementList '}'        # braces are mandatory: no one-line bodies
```
- **Braces are mandatory** for every control-statement body and every case entry. A missing `{` is `parse.expectedBraces`.
- **Conditions** are ordinary expressions without surrounding parentheses. A block literal in operand position inside a condition is `parse.blockInCondition`, because its `{` would be ambiguous with the body. Parenthesise that part: `if (items anySatisfy: { :e | e }) { … }`.
- **`of`** is not reserved (`array of: Part` must keep working). After a `case` subject, the identifier `of` followed by `{` ends the subject instead of being read as a unary send.
- **Case labels** are members, constants, types or parenthesised ranges: `Color red, Color green: { … }`, `0: { … }`, `(1 to: 9): { … }`, `int32: { :n | … }`. The lexer turns `green:` into a keyword token, and inside the entries the parser splits it back into the label and the colon.
- `x := 4;` is an assignment syntactically. Whether it declares `x` (implicit declaration, design §3.3) is decided in semantic analysis.
- **Group or block?** A `{ … }` at the start of a statement is a block *receiver* only if a message (identifier, keyword, operator) follows **on the same line** as its closing `}`. Otherwise it's a statement group and the next line starts a new statement. That way `@[overflow: wrap] { … }` followed by `x: int32;` on the next line isn't read as a keyword send. (Control statements aren't affected: their bodies are always statement lists.)
- **Forgotten `;`.** An identifier followed by `:=` is never consumed as a unary selector, so `x := 1` followed by `y := 2;` reports the missing `;` and continues.

## Expressions
```
expression  = binaryExpr { keywordSelector binaryExpr }             # one keyword send; selector = all parts
binaryExpr  = unaryExpr { binaryOp unaryExpr }                       # precedence table below, left-assoc
unaryExpr   = postfix { identifier }                                 # unary sends bind tightest
postfix     = primary { '.' identifier }                             # field access / qualification
primary     = literal | identifier | 'self' | 'super' | 'nil' | 'true' | 'false'
            | '-' number | '(' expression ')' | block | array | attributes '(' expression ')'
block       = '{' [ {':' blockParam} '|' ] statementList '}'
blockParam  = identifier [':' typeTerm] | keywordSelector typeTerm     # :i  :i: int32  :i : int32
array       = '[' [expression {',' expression} [',']] ']'
```
| Precedence (high → low) | Operators |
|---|---|
| 7 | `<<` `>>` |
| 6 | `*` `/` `%` `&` |
| 5 | `+` `-` `\|` `^`, and any other operator |
| 4 | `=` `~=` `==` `<` `<=` `>` `>=` |
| 3 | `and` |
| 2 | `or` |
| 1 | `??` |

`?` is only valid as an optional-type suffix. `-` directly before a number in operand position is a negative literal (`a -1` is still subtraction).


## Types
```
type        = typeTerm [ keywordSelector typeTerm { keywordSelector typeTerm } ]   # type message
typeTerm    = '[' typeList ']' ['?']
            | qualifiedName ['?']                                   # type name
            | qualifiedName identifier {identifier}                 # constant: Day mon
            | number | char | '-' number                            # constant: 0, 'a'
typeList    = [keywordPart] type { ',' [keywordPart] type }
```
- One unnamed element in brackets is just grouping (`[array of: int32]`). Several elements, or a named one, make a tuple (`[int32, string]`, `[count: int32, name: string]`). Parentheses aren't allowed in types.
- Type messages: `array of: Part`, `array of: uint8 by: [0 to: 511]`, `hashtable keys: K values: V`, `set of: Perm`, `pointer to: uint32`, subranges `0 to: 100` and `Day mon to: Day fri`.
- A bare constant (`x: 5;`) is `parse.expectedType`.

## Attributes
```
attributes  = '@[' [attribute {',' attribute}] ']' { '@[' … ']' }
attribute   = name [':' binaryExpr] | name '.' … keywordSelector binaryExpr      # static, align: 16, reflect.requires: invoke
```
Attributes attach to the unit, items, class members, fields, statements (including groups), and parenthesised expressions.

## Error recovery
Every list (items, class/record/extends members, fields, enum members, statements, case entries) recovers independently. On a syntax error the parser reports one diagnostic, skips to the next `;` or the end of a balanced `{ … }` at the same level, and records an `Error` node covering the skipped text. The rest of the file is still parsed and the tree is always complete.
