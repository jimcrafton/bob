# bob semantics (as implemented)

What the analyzer (`bobSemantic`, `src/semantic`) and code generator (`src/compiler/codegen`) accept and produce as of phase 2. `docs/design.md` is the intent; this file is the current behaviour. Anything not listed here is reported as `sema.unsupported`, with the phase it's planned for.

## Programs and messages
- An executable is a `program name, version { … }` unit with an `extends name { … }` block. Messages there are **class-side** and must be marked `@[static]`. Inside them `self` is the program, so `self fact: 5` and `factorial fact: 5` are the same direct call.
- The entry point is `@[static] msg main: args:array [int32]` (or `msg main [int32]`). Its return value is the process exit code. `args` can be declared but not used until arrays exist (phase 5).
- Messages are compiled to internal LLVM functions named `bob.<program>.<selector>`. The runtime calls `bobMain`, which the compiler generates.
- **Parameters and `for` loop variables are read-only.** Assigning to them is `sema.assignToReadOnly`.
- A message with a return type must return on every path (`sema.missingReturn`). `loop { }` without a `break` counts as returning.

## Types
| Type | Notes |
|---|---|
| `bool` | `true`, `false` |
| `int8 int16 int32 int64`, `uint8 … uint64` | `byte` = `uint8`, `intptr`/`uintptr` = 64-bit |
| `real32 real64` | IEEE |
| records | value types, C layout in declaration order, `inherits` places the base fields first, `@[packed]` supported |

| blocks | closures, see [Blocks](#blocks-phase-3) |

Not yet: `int128`, `char`, strings, arrays and other containers, optionals, tuples, enums, subranges, named types, classes, protocols.

### Implicit conversions
Only lossless ones: a wider integer of the same signedness, an unsigned integer to a strictly wider signed one, `real32` to `real64`. Everything else is explicit.

### Literals
An untyped literal takes its type from context: the declared type, the parameter type, or the other operand (`x + 1`, `1 + x`, `3000000000 + big`). Otherwise integers default to `int32` and reals to `real64`. A literal that doesn't fit its type is `sema.literalOutOfRange`. Suffixes force a type: `10u8`, `5i64`, `2.0r32`.

### Variables
- `x: T;` is zero-initialised. `x: T := e;` initialises it.
- `x := e;` with an unknown `x` declares it with the type of `e`. `e` is checked first, so `x := x + 1` with an unknown `x` is `sema.undeclared`. Assigning to an existing name never redeclares it.
- Names can't be redeclared in the same scope. An explicit declaration in an inner block may shadow an outer variable.
- A value with no runtime type (a type name, the program, a message with no return value) can't initialise an inferred variable (`sema.cannotInfer`).

## Expressions
| Form | Operand types | Result |
|---|---|---|
| `+ - * /` | numeric, both sides converted to their common type | common type |
| `%` | integers | common type |
| `& \| ^` | integers | common type |
| `= <> < <= > >=` | numeric (`=` and `<>` also on `bool`) | `bool`; for reals `<>` is true when either side is NaN |
| `and`, `or` | `bool` | `bool`, **short-circuit** |
| `x negated`, `x abs` | signed integer or real (`abs` also unsigned) | same |
| `b not` | `bool` | `bool` |
| `x asInt8 … asUInt64 asReal32 asReal64` | numeric | target type, **checked** |
| `a max: b`, `a min: b` | numeric | common type |
| `a wrapAdd:`/`wrapSub:`/`wrapMul: b` | integers | always wraps |
| `a satAdd:`/`satSub:`/`satMul: b` | integers | always saturates |
| `x shl n`, `x shr n` | integer value, any integer amount | value's type; `shr` is arithmetic for signed types |
| `Point x: 1 y: 2` | every field, in declaration order | the record |
| `p.x` | record | field type; assignable when `p` is a variable |
| `self m: …`, `prog m: …` | per the message's parameter types | its return type |


## Statements
`if`/`else if`/`else`, `while`, `loop`, `break`, `continue`, `return`, statement groups, and:
- `for i in a to: b { … }` visits `a`…`b` inclusive (nothing if `a > b`), and it's correct up to the type's maximum (`250u8 to: 255u8`). The bounds must be integers. The loop variable has their common type, or the declared type (`for b: uint8 in …`). Intervals aren't values yet: `k := 1 to: 3` is `sema.intervalOutsideFor`.
- `case x of { … } else { … }` on integers. Labels are integer literals or `(lo to: hi)` ranges and must not overlap. Because an integer can't be covered completely, `else` is required (`sema.caseNotExhaustive`). Enums and unions come in phase 5.

## Blocks (phase 3)
- **Values.** `{ :a :b | statements }` is a closure. Its value is its last statement when that's an expression, so `{ :i: int32 | i * i }` returns `i * i`. Call it with `value`, `value:`, `value:value:` and so on.
- **Types** are written as a type message when needed: `[block takes: [int32, int32] returns: bool]`, `[block takes: [int32]]` (no result), `[block returns: int32]` (no parameters), or plain `block`.
- **Inference.**
  - A message parameter typed plain `block` gets its type from how the body uses it. `f value: x` fixes the parameter types, and a result used as `int32` fixes the result. Passing it to another message's block parameter fixes it from there.
  - If the body doesn't pin it down (never invoked, or only in a cycle of messages), you get `sema.cannotInferBlock`, asking for the annotation.
  - A block literal's parameter types come from the expected block type. Without one, write them, as in `{ :i: int32 | … }`.
  - A local declared `b: block := …` takes its type from the initializer.
- **Capture by reference.** A block reads and writes the variables it uses from enclosing code directly. Its environment is a stack record of pointers to those variables, with no allocation.
- **No escaping (until phase 8).** A block can be passed as an argument, held in a local, and invoked. It can't be a message's return type or a record field (`sema.unsupported`), so it never outlives the frame that created it.
- **Inlined messages.** These compile to plain branches and loops, with no closure:
  - `b ifTrue: { … }`, `ifFalse:`, `ifTrue:ifFalse:`, `ifFalse:ifTrue:` with block literals. The two-branch forms produce a value when both branches end in expressions of a common type: `m := (x > 3) ifTrue: { 100 } ifFalse: { 200 };`.
  - `a to: b do: { :i | … }` behaves like `for i in a to: b`.
  - `n timesRepeat: { … }`.

  Non-literal arguments are `sema.inlinedNeedsBlock`. `break`/`continue` inside `to:do:` and `timesRepeat:` apply to them, and inside the `ifTrue:` family they apply to the surrounding loop.
- **`return`** is allowed only in code that runs in the message's own frame: its body and inlined blocks. Inside any other block it's `sema.returnInBlock`, because it would have to unwind through whoever called the block. Make the value the block's last expression instead. Likewise, `break`/`continue` can't leave a block function.
- **Code generation.** A non-inlined block becomes an internal function `bob.<program>.<message>.blockN(env, args…)`. A block value is `{function pointer, environment pointer}`, and calling it is an indirect call. `bobc --explain-dispatch` lists every send and block with how it was compiled.

## Overflow and traps
- `+ - *`, `negated`, `abs` and `shl` follow the overflow mode: `trap` (default), `wrap` or `saturate`. It's set with `@[overflow: …]` on the unit, a message, a statement group or a parenthesised expression, and the innermost wins.
- Division and remainder by zero always trap. Signed `MIN / -1` traps in `trap` mode, gives `MIN` in `wrap` and `MAX` in `saturate`. `MIN % -1` is `0` in the non-trapping modes.
- A shift amount below 0 or at least the bit width always traps.
- `asX` conversions trap when the value doesn't fit (including NaN and out-of-range reals). Real→integer truncates toward zero.
- A trap calls the runtime's `bobRuntimeTrap(reason)`, which prints `bob: runtime trap: <reason>` to stderr and **exits with code 101** (the code Rust uses for panics). Reasons: `integer overflow`, `division by zero`, `conversion out of range`, `shift amount out of range`.

## For tools
`sema::SemanticModel` keeps every result keyed by AST node id:
- `expressionTypes`: the type of every expression;
- `localRefs`: the variable behind an identifier, declaration, implicit-declaring assignment or `for`;
- `sends`: how each send is implemented (kind, operand type, target message or record, overflow mode);
- `fieldRefs`, `caseRanges`, `implicitDeclarations`.

`analyze()` always completes and leaves a partial model even when there are errors. The library has no LLVM dependency.
