# bob semantics (as implemented)

What the analyzer (`bobSemantic`, `src/semantic`) and code generator (`src/compiler/codegen`) accept and produce as of phase 7. `docs/design.md` is the intent; this file is the current behaviour. Anything not listed here is reported as `sema.unsupported`, with the phase it's planned for.

## Programs and messages
- An executable is a `program name, version { … }` unit with an `extends name { … }` block. Messages there are **class-side** and must be marked `@[static]`. Inside them `self` is the program, so `self fact: 5` and `factorial fact: 5` are the same direct call.
- The entry point is `@[static] msg main: args:array [int32]` (or `msg main [int32]`). Its return value is the process exit code. `args` is an `array of: string` holding the command-line arguments after the program name.
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
| classes, protocols, `C?` | references, see [Classes](#classes-protocols-and-arc-phase-4) |
| enums, subranges, named types | see [Enums](#enums-subranges-and-named-types-phase-5) |
| `T?` | optionals of any value type, see [Optionals](#optionals-phase-5) |
| `string`, `char` | see [Strings](#strings-phase-5) |
| `array`, `dictionary`, `hashtable`, `set`, fixed arrays | see [Containers](#containers-phase-5) |
| `A \| B` | inferred unions, see [Unions](#inferred-unions-phase-5) |

Not yet: `int128`, tuples, `slice`, `pointer`, distinct record and class types.

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
- `for i in a to: b { … }` visits `a`…`b` inclusive (nothing if `a > b`), and it's correct up to the type's maximum (`250u8 to: 255u8`). The bounds must be integers. The loop variable has their common type, or the declared type (`for b: uint8 in …`). Intervals aren't values yet: `k := 1 to: 3` is `sema.intervalOutsideFor`. `for` also iterates enums, subranges, enum intervals (`Color red to: Color blue`), string views and containers (below).
- `case x of { … } else { … }` on integers and `char`. Labels are integer literals or `(lo to: hi)` ranges and must not overlap. Because an integer can't be covered completely, `else` is required (`sema.caseNotExhaustive`). A `case` over an enum, a subrange or a union can cover every value and leave `else` out (below).

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

## Classes, protocols and ARC (phase 4)
**Declaring**
- `class Name [inherits Base] [implements P, Q] { public { … } private { … } msg … }`.
- Messages without `@[static]` are instance messages, where `self` is the receiver. `@[static]` messages are class-side (`Point x: 1 y: 2`).
- `extends Name [implements P] { msg … }` adds messages and conformances to a class in the same unit.
- A concrete class can be subclassed only in its own module (design §5.4).

**Creating**
There's no `new` (design §4.3a).
- `p: Point;` constructs a `Point` with every field defaulted. This is the only form that builds a default object; there's no expression for it.
- `Point x: 4 y: 7` is a **constructor message** generated from the class's **public** fields, including inherited ones:
  - Pass any of them, in declaration order (`sema.constructorOrder` otherwise). Private fields can't be passed.
  - A user-declared `@[static]` message with the same selector takes precedence.
  - `x := Point new` is `sema.unknownMessage`, with an explanation.
- **Order of construction:**
  1. Allocate a zero-filled object.
  2. Store the fields that were passed.
  3. Run the initializers of the other fields, superclass fields first. A field with no initializer gets its type's default: 0, the first enum member, the low bound of a subrange, nil, an empty string or container, or, for a non-optional class field, a **default-constructed** object of that class.
  4. Send each class's `init`, superclass first.
- **Required fields.** A non-optional class field with no initializer is *required* when its class can't be default-constructed: it's a protocol, it has required fields itself, or it would contain itself (`next: Node`).
  - A constructor message must pass every required field (`sema.missingConstructorArgument`).
  - `x: C;` is `sema.uninitializedReference` when C has required fields, and the error shows the constructor to write.
  - A *private* required field is an error at its declaration, because nothing outside the class could set it.
- **`msg init { … }`** takes no arguments and returns nothing (`sema.invalidInit` otherwise, also for `init:` selectors). It runs automatically for each class that declares one, superclass first. It sees every field and can set private ones. It isn't a message you can send.
- **`copy`** is generated for every class that doesn't declare its own:
  - It makes a new object of the receiver's dynamic class with the same field values. References and strings are shared and retained, not deep-copied.
  - It then runs `init` again.
  - It has a vtable slot, so `(a: Animal) copy` copies a `Bird`.
- Construction that can fail, or that returns an existing object, is an ordinary `@[static]` message.

**Where objects live** (`bobc --explain-dispatch` reports each allocation)
- An object created by `x: C;` or `x := C f: …` goes **on the stack** when it provably doesn't outlive the function that creates it:
  - Nothing it's assigned to escapes. Escaping means it's stored into a field or container, returned, passed to a block, used as a key, or passed to a message that keeps that parameter or `self`.
  - Each message's summary of what it keeps is computed over the whole program, to a fixed point, so recursion is handled. Vtable and protocol sends use every implementation that could run.
  - No `init` in the class chain keeps `self`.
  - Every variable it's assigned to is declared in the same function and the same loop body. That lets the next pass of a loop, or the frame's exit, reuse the memory safely.
  - The object is at most 4 KB (larger ones need `@[stack]`).
- Everything else goes on the heap, including `copy` and other message results, which are built inside the message.
- **No reference counting on stack objects.** A local whose every assignment is a stack object, nil, or another such local (`SemanticModel::stackOnlyLocals`) gets no retain or release calls at all: not on stores, loads, `ifNotNil:` bindings or function exit. A stack object reached some other way (as `self` inside its own messages, or through a parameter) still sees the ordinary calls, but it carries the immortal flag, so they return at once. It never appears in the leak report. Its fields are released when the function returns, or when a loop runs its declaration again.
- `@[stack] x := C f: …;` *requires* stack placement: if the object escapes, it's `sema.stackEscape` at the point where it escapes, with the reason. `@[heap]` keeps it on the heap.
- Both attributes apply only to declarations that create an object (`sema.invalidAttribute` otherwise). Records and other values already live in the variable.

**Fields**
- Inside the class (and its subclasses), fields are reached by bare name (`count := count + 1`) or with `.` on another instance (`other.x`).
- Outside, `.` is `sema.privateField`. Public fields get synthesized accessors instead (`p x`, `p x: 5`), compiled to inline loads and stores. A message with the same selector takes precedence.

**`nil` and optionals**
- A non-optional class reference is never `nil`. A local or field declared without a value is default-constructed (see **Creating**), or it's `sema.uninitializedReference` when that isn't possible. A protocol-typed local needs a value (`sema.newProtocol`).
- `Name?` may be `nil`.
- `x = nil` and `x <> nil` test it.
- `x ifNotNil: { :value | … }`, `ifNil:`, `ifNil:ifNotNil:` and `ifNotNil:ifNil:` unwrap it, inlined.
- Sending other messages to an optional is `sema.optionalReceiver`.
- Optionals of non-class types come later.

**Equality and records**
- `=` and `<>` on references compare identity.
- Records can't hold references (`sema.referenceInRecord`), since records are strictly data.

**Protocols**
- `@[protocol] class P { msg area [int32]; msg describe [int32] { … } }` declares required messages (no body) and defaults (body).
- No fields, no `new`, no class-side messages.
- `@[protocol] class Q inherits P` refines P.
- A conforming class must define every required message with exactly the declared signature (`sema.missingRequiredMessage`, `sema.overrideMismatch`). Defaults it doesn't define are adopted.
- Overrides in subclasses must also keep the signature.

**Dispatch** (`--explain-dispatch` shows it per send)
| Receiver's static type | Selector | Compiled to |
|---|---|---|
| class `C` | one implementation in `C` and everything below it | **tier 1**: direct call |
| class `C` | overridden somewhere below `C` | **tier 1v**: load `isa`, call through vtable slot |
| protocol `P` | any | **tier 2**: `bob_msgSend(self, selector, args…)` |
| `super` | any | direct call to the superclass's implementation |

- Instance messages compile to `bob.<Class>.<selector>(self, selector, args…)`. Protocol defaults compile to `bob.<Protocol>.<selector>`, which dispatches sends to `self` dynamically.
- Every class gets a vtable slot for each of its instance messages, with inherited slots first.
- Messages that can be reached through `bob_msgSend` (protocol messages) can't return records yet.

**Object layout and runtime**
- An object is `{class*, refCount: uint32, flags: uint32, fields…}`, with inherited fields first and the object allocated from the process heap.
- The class info (`BobClass` in `src/shared/objectModel.h`) holds the superclass, name, instance size, `destroy` function, method table `{selector, implementation}` and the cache pointer, followed by the vtable.
- Selectors are unique strings, compared by address.
- `bob_msgSend` (MASM) probes a per-class cache and falls back to `bobMsgSendLookup`, which walks the superclass chain. An unknown selector traps.

**Reference counting (ARC)** (atomic counts)
- Variables and fields own one reference. Parameters and `self` are borrowed.
- Message results and `new` are owned (+1).
- An owned value that's only used within a statement (an argument, a receiver, a result that's ignored) is released when the statement ends.
- Assignments retain the new value and release the old one.
- A function releases its class-typed locals when it returns. Variables declared in a loop release their previous value on each new declaration.
- Values loaded from fields, or from variables captured by a block, are retained while in use, because the callee could overwrite their source.
- When the count reaches 0, the class's `destroy` releases the object's reference fields and the memory is freed.
- Cycles aren't collected. `weak`/`unowned` come with escaping closures in phase 8.
- **Leak report:** at exit, the runtime prints `bob: leak: N object(s), M bytes still allocated at exit` to stderr if anything is still alive. The test harness requires every program to report nothing, except `leakCycle.bob`, which checks that the report works.

## Enums, subranges and named types (phase 5)
**Enums** (`enum Color { red; green; blue; }`, `enum PciClass : uint8 { storage := 0x01; … }`)
- Storage is `int32` unless given. Values count up from 0 or from the previous explicit value. An explicit value must be an integer literal that fits the storage. Duplicate values are `sema.duplicateEnumValue`.
- Members are always qualified (`Color red`). An enum is its own type with no conversion to or from integers. `=`, `<>`, `<` … compare values of the same enum.
- Stored as the member's **value** (C layout). `ordinal` is the position, and differs from `value` when values are sparse.

| Message | Result |
|---|---|
| `Color red`, `Color first`, `Color last` | compile-time constants |
| `Color count` | `int32` constant |
| `c value` / `c ordinal` | storage integer / `int32` |
| `c succ`, `c pred` | next or previous member; traps past the ends (`no next or previous member`) |
| `c name`, `c printString` | the member's name (an immortal string, nothing allocated) |
| `Color fromValue: n` | `Color?`: nil when no member has that value |
| `for c in Color { }`, `for c in Color red to: Color blue { }`, `Color do: { :c \| }` | loops over members in order |

- A variable of an enum type with no initializer starts as the **first member**, not 0.

**Sets of enums** (`p: set of: Perm := [Perm read, Perm write];`) are bitmasks: bit *n* is the member whose value is *n*. The width is the smallest of 8/16/32/64 bits that holds the largest value, or the storage if that's wider.
- `+` union, `-` difference, `*` intersection, `=`, `<>`.
- `includes:`, `count` (`int32`), `isEmpty`, and `value` (the raw mask, unsigned).
- `add:`/`remove:` change a variable or field in place.
- `(set of: Perm) fromValue: bits` gives a `set?`, nil when unknown bits are set.
- `for m in p` visits members in ordinal order.

**`case`** over an enum, an enum subrange or an integer subrange may leave out `else` if its labels cover every value. Otherwise it's `sema.caseNotExhaustive`, which names up to five missing members. Labels are members, constants or ranges `(Day mon to: Day fri)`, and must not overlap. A `case` that covers everything counts as returning when every entry returns.

**Subranges** (`type Percent := 0 to: 100;`, `type Port : uint16 := 1 to: 65535;`, `type Weekday := Day mon to: Day fri;`, inline `pct: 0 to: 100`)
- Bounds are integer literals or members of one enum.
- Storage is the smallest of `uint8 … uint64` (or `int8 …` when the low bound is negative) unless given with `: T`.
- **Out is free**: a subrange widens to its base like the base does. Arithmetic and comparisons use the base (`pct + 1` is `uint8`). `x := pct` keeps the subrange.
- **In is checked**. A literal outside the range is a compile error. Any other integer (or member, for enum subranges) is checked at run time and traps with `value out of range`.
- `Percent fromValue: n` gives `Percent?`.
- `Percent first` / `last` / `count`, and `for i in Percent { }`.
- A subrange variable with no initializer starts at the low bound. This includes fields of records and classes.

**Named types** (`type PhysAddr := uint64;`)
- A named numeric, `bool` or `char` type is **distinct**. It has the base's storage and operators, but mixes only with itself: `p + q` is a `PhysAddr`, untyped literals adopt it, and assigning a `uint64` or a `VirtAddr` is `sema.typeMismatch`.
- `p value` gives the base, and `PhysAddr fromValue: n` makes one.
- `@[alias] type Handle := uint64;` is just another name for `uint64`.
- Distinct record and class types aren't supported yet.

## Optionals (phase 5)
- Any value type can be optional: `int32?`, `Color?`, `string?`, `[array of: int32]?`.
- A class optional is still a nullable pointer. Every other optional is `{present, value}`, and an absent one holds the zero value.
- `T` converts to `T?` implicitly, and `nil` converts to any optional. `x = nil` and `x <> nil` test it. Two optionals that aren't class references can't be compared with each other; unwrap first.
- `ifNotNil:`, `ifNil:`, `ifNil:ifNotNil:` and `ifNotNil:ifNil:` unwrap any optional. The two-block forms produce a value when both blocks end in expressions of a common type: `n := (d at: "k") ifNil: { 0 } ifNotNil: { :v | v };`.

## Strings (phase 5)
- `string` is an immutable, reference-counted UTF-8 buffer, so copying is free and there's nothing to copy on write. Its bytes are always valid UTF-8.
- The empty string is a null pointer. Literals are immortal constants in read-only data: no allocation and no refcount traffic.
- `char` is a Unicode scalar value (`'é'`, stored as `uint32`): `c value`, `c asString`, `char fromValue: n` (`char?`), and comparisons.
- There's no `size`, `length`, `count` or integer `at:` on a string (`sema.unknownMessage` says which view to use). Pick a view:

| View | Elements | `count` | Indexing |
|---|---|---|---|
| `s bytes` | `uint8` | bytes | `s bytes at: n` (bounds-checked) |
| `s codepoints` | `char` | Unicode scalars | — |
| `s graphemes` | `string` | extended grapheme clusters (UAX #29, Unicode 16.0) | — |

  Views are iterable with `for` and `do:`. A view is the same string, so no copy is made.
- `+` concatenates two strings (`"n = " + n printString`; other types need `printString`).
- `=` and `<` compare byte-wise, which for UTF-8 is codepoint order.
- `startsWith:`, `endsWith:`, `includesSubstring:`, `isEmpty`.
- `s split: ","` gives `array of: string` (empty pieces are kept), and `", " join: parts` joins one.
- `x printString` works on numbers, `bool`, `char`, enums (the member name), strings and unions of these. Reals print up to 15 significant digits, so the output isn't yet the shortest form that round-trips.
- `s toUtf8` gives `array of: uint8`. `string fromUtf8: bytes` gives `string?`, nil if the bytes aren't valid. `string fromUtf8Lossy: bytes` replaces each invalid byte with U+FFFD.
- The grapheme tables are generated from the UCD by `tools/unicodegen` (see building.md). The runtime's segmenter passes all of Unicode's `GraphemeBreakTest.txt` (`Unicode.GraphemeBreakConformance`).

## Containers (phase 5)
Element types are inferred. You write `array of: T` (or a similar type message) only when the compiler asks for it.

| Type | Written | Notes |
|---|---|---|
| `array` | `[1, 2, 3]`, `a: array;`, `array of: T` | 0-based, bounds-checked (`index out of range`) |
| fixed array | `array of: T by: Opcode`, `array of: uint8 by: [0 to: 511]` | stored inline, `at:` takes the index type, so there's no bounds check |
| `dictionary` | `d: dictionary;`, `dictionary of: V` | keys are always `string` |
| `hashtable` | `h: hashtable;`, `hashtable keys: K values: V` | keys are integers, enums, `char`, `bool` or strings |
| `set` | `s: set of: string := ["a"];`, `s: set;` | a bitmask when the element is an enum (above) |

**Inference**
- From a literal: `[1, 2]` is `array of: int32`. A typed element steers untyped literals, so `[1, 2.5]` is `real64`.
- From the first store into a bare `array`/`dictionary`/`hashtable`/`set` local, field or `[array]` result: `add:`, `at:put:`, an assignment, or a returned value. Later stores must match.
- Reading the elements before anything is stored is `sema.cannotInferElement`. So is never storing anything.
- Message **parameters** must spell out the element type (`nums:[array of: int32]`). Specialising a message per element type at each call site (design §4.5) is still to come.
- **Mixed literals**: classes that share a superclass or protocol give that type. Otherwise the element type is the closed union of the element types (see below).

**Value semantics**
- Containers are values. `b := a` shares one buffer until either variable changes it, which copies it first.
- An empty container is a null pointer, so declaring one allocates nothing.
- Mutating sends need a variable or field as the receiver (`sema.notAssignable`).
- `for` and `do:` keep their own reference to the container while they run, so the body can reassign the variable safely.

| Messages | Containers |
|---|---|
| `count` (`int64`), `isEmpty` | all |
| `at:`, `at:put:` | arrays and fixed arrays (index), dictionaries and hashtables (key; `at:` gives `V?`) |
| `add:`, `includes:`, `first`, `last`, `removeLast` | arrays (`first`/`last`/`removeLast` trap when empty) |
| `at:ifAbsent: { default }`, `includesKey:`, `removeKey:`, `keys`, `values`, `keysDo:`, `keysAndValuesDo: { :k :v \| }` | dictionaries and hashtables |
| `add:`, `remove:`, `includes:` | sets |
| `do: { :each \| }`, `for x in c { }` | arrays, fixed arrays, sets; `do:` on dictionaries and hashtables visits values |

- `for` over a dictionary or hashtable is `sema.notIterable`. Iterate `d keys` or `d values`, or use `keysAndValuesDo:`.
- `main:`'s `args` is an `array of: string`: the command-line arguments after the program name, split by the Windows rules and converted to UTF-8.

**Runtime representation**
- Strings, arrays and tables are runtime buffers with an object header (`src/shared/objectModel.h`), counted by the leak report.
- Generated code describes each element type to the runtime with a `BobElementType`: its size, retain/release callbacks, and whether equality and hashing use the raw bytes or string contents.

## Inferred unions (phase 5)
- A literal that mixes unrelated types, such as `[7, 8, "oiudf", 1.5]`, has a closed union as its element type (`int32 | real64 | string`).
- Each value is stored as a tag plus the value inline, so primitives aren't boxed.
- Messages every member understands: `printString` compiles to a switch on the tag with a direct call per member.
- Anything else needs narrowing first:
  - `u ifKindOf: int32 do: { :n | … }`
  - `u isKindOf: string`
  - `case u of { int32: { :n | … } string: { :s | … } }`, which binds the narrowed value when an entry has one label, and must cover every member or have an `else`.
- Using a union where a specific type is needed is an error naming the union.
- A union member can't be a class reference or optional.

## Compile time (phase 6)
Compile-time code is ordinary bob, type checked like everything else. `bobc` compiles it with the same code generator and runs it inside the compiler (an LLVM ORC JIT) against a build of the runtime linked into the compiler (`bobrtHosted`). Its results are embedded in the program.

**Constants**
- `@[constant] NAME := expr;` or `@[constant] name: T := expr;` at unit level declares an immutable constant, computed during compilation.
  - With a type, the value converts to it as an initializer would (`@[constant] mask: uint64 := 255u8;`).
  - The name's case doesn't matter.
  - `@[compiletime]` on the value is allowed but redundant.
  - A constant needs a value (`sema.invalidConstant`).
- A unit-level declaration without `@[constant]` is `sema.unitLevelVariable`: mutable globals aren't supported yet.
- Constants are visible in every message, and may refer to each other in any order. They're evaluated in dependency order, and a cycle is an error (`sema.constantCycle` or `compiletime.cycle`).
- Assigning to one is `sema.assignToConstant`. A constant's value must be **embeddable**: numbers, `bool`, `char`, enums, subranges, enum sets, strings, records, fixed arrays, optionals and unions of those, or arrays, sets, dictionaries and hashtables of those (`sema.notEmbeddable` otherwise; objects and blocks exist only while code runs).
- **How constants are embedded:** strings and arrays become immortal read-only objects shared by every use (a 256-entry table exists once in the binary). Records and scalars become immediate constants. Dictionaries, hashtables, hash sets and unions are rebuilt where they're used.
- Constants have value semantics like any container: `mine := TABLE; mine add: x;` copies first.
- Any other statement at unit level must be in a `@[compiletime] { … }` block (`sema.unitLevelCode`).

**`@[compiletime]`**
- `@[compiletime] msg …` can be sent only from compile-time code (`sema.compileTimeOnly`). It isn't compiled into the program.
- `@[compiletime] (expr)` inside a message is computed once, while compiling, and its value is embedded.
- `@[compiletime] { … }` (at unit level or in a message) runs while compiling and leaves nothing behind. It's meant for checks such as layout asserts.
- Compile-time code sees constants, types, and messages. It doesn't see the variables of the message it's written in (`sema.undeclared`), because those don't exist yet.
- Ordinary messages can be called from compile-time code too.

**`compiler`**: an object that exists only in compile-time code (`sema.compilerOutsideCompileTime` elsewhere).
- `compiler error: "…"` fails the compilation with that message, reported at the line that sent it (`compiletime.error`).
- `warning:` and `note:` report without failing.

**Failures:** compile-time code that traps (`division by zero`, `index out of range` …), or runs for more than 20 seconds, makes the compilation fail with `compiletime.failed`, naming the constant or block. The compiler itself isn't affected.

**`meta`** is a reserved unary message on any value or type. It's always answered from static types, and never evaluates its receiver.

| Query | Result |
|---|---|
| `x meta size`, `x meta alignment` | `int64` bytes, as laid out on the target (a class gives its object size, header included) |
| `x meta typeName` | `string`, e.g. `"Point"`, `"array of: int32"`, `"int32 \| string"` |
| `x meta name` | the variable's, field's or type's name |
| `x meta type` | the same information, for the type (so `self meta type fields` works) |
| `T meta count` | members of an enum, elements of a fixed array, values of a subrange, fields of a record |
| `Color meta members` | `array of: Color` (a union gives its member type names) |
| `c meta elementType` | `meta` of a container's or optional's element type |
| `T meta fields` | `array of: FieldMeta` (`name`, `typeName`, `offset`, `size`), only in compile-time code. Outside the class, only public fields |

- The layout rules live in the semantic library (`layout.h`).
- `Layout.MatchesTheCodeGenerator` checks them against LLVM's for every kind of type.
- Not yet: unrolling compile-time loops over `meta fields` into code (splicing), `compiler target`, `compiler readFile:`, and synthesising messages or classes.

## Modules and libraries (phase 7)
The model is Object Pascal's units (design §11.5).

**Units and sections**
- `module name, 1.4.11` and `lib name, 2.0.0` units are compiled on their own.
- Their unit-level declarations go in an `interface { … }` section, which is what importers see, or `implementation { … }` (or no section), which stays private.
- An interface declaration can't use an implementation type (`sema.privateInInterface`). That includes the private fields of an exported class, because importers need its layout. It also rules out an inferred union, which can't be written as a type, and a dictionary or hashtable constant.
- A program has no sections (`sema.sectionInProgram`).

**Building**
- `bobc geometry.bob` writes `geometry.lib`, a static library.
- `bobc -c geometry.bob` writes `geometry.obj` instead.
- Both also write `geometry.bobi`, the interface:
  - the interface declarations in bob syntax, without bodies;
  - every type spelled out, and every name qualified by its module;
  - constant values as literals;
  - escape summaries, as `@[keeps: "self p"]`;
  - field initializers marked `@[initialized]`;
  - protocol defaults marked `@[default]`;
  - a header with its own fingerprint and those of the interfaces it was compiled against.
- Importers read it with the ordinary parser and analyzer. It never needs editing.

**Importing**
- `import geometry, 1.4;` looks for `geometry.bobi` in the source file's directory, then in each `--module-path=` directory (`sema.importNotFound`).
- `import x, 1.4;` accepts 1.4.0 and later with the same major version (`sema.importVersion`).
- Imports are transitive.
- A program links the `.lib` (or `.obj`) next to every interface it loaded.
- If a module was compiled against a different interface of one of its dependencies than the one now present, compilation fails: `sema.staleModule` names the module to recompile.
- A change to implementation code only doesn't change the fingerprint, so nothing else needs recompiling.

**Names**
- Imported names are used unqualified.
- A unit's own declarations shadow imported ones.
- When two imports declare the same name, unqualified use is `sema.ambiguousName`. Write `a.Point` (types and constants, in type positions and expressions).
- A module's messages are sent to the module (`geometry quadrantOf: p`).

**Across the boundary**
- Imported classes, records, enums, named types, constants, protocols and module messages work as if declared locally.
- Calls to imported code are direct, vtable or protocol sends, exactly as within a module. The interface gives the class hierarchy and the vtable order.
- Imported objects can be placed on the stack when the interface's escape summaries allow it.
- Compile-time code can call imported messages and build imported objects; the JIT loads the imported libraries.
- A concrete class can't inherit an imported concrete class (`sema.closedClass`), but it can implement an imported protocol.
- `extends` on an imported class isn't supported yet.
- Symbols carry their unit (`bob.geometry.Square.area`, `class.geometry.Square`).
- Selectors are emitted once per module and merged by the linker, so protocol sends work across modules.

## Overflow and traps
- `+ - *`, `negated`, `abs` and `shl` follow the overflow mode: `trap` (default), `wrap` or `saturate`. It's set with `@[overflow: …]` on the unit, a message, a statement group or a parenthesised expression, and the innermost wins.
- Division and remainder by zero always trap. Signed `MIN / -1` traps in `trap` mode, gives `MIN` in `wrap` and `MAX` in `saturate`. `MIN % -1` is `0` in the non-trapping modes.
- A shift amount below 0 or at least the bit width always traps.
- `asX` conversions trap when the value doesn't fit (including NaN and out-of-range reals). Real→integer truncates toward zero.
- A trap calls the runtime's `bobRuntimeTrap(reason)`, which prints `bob: runtime trap: <reason>` to stderr and **exits with code 101** (the code Rust uses for panics). Reasons: `integer overflow`, `division by zero`, `conversion out of range`, `shift amount out of range`, `value out of range` (into a subrange), `no next or previous member` (`succ`/`pred`), `index out of range`.

## For tools
`sema::SemanticModel` keeps every result keyed by AST node id:
- `expressionTypes`: the type of every expression;
- `localRefs`: the variable behind an identifier, declaration, implicit-declaring assignment or `for`;
- `sends`: how each send is implemented (kind, operand type, target message or record, overflow mode);
- `fieldRefs`, `caseRanges`, `implicitDeclarations`, `exhaustiveCases` (a `case` that covers every value);
- `enums` and `namedTypes`. A `for` over a collection records its `collectionRange` send on the `for` statement, because the iterable may itself be a send (`s bytes`).

Containers declared bare (`b: array;`) start as *pending* types. Once the element type is inferred, `Type::resolved` points to the final type. `typeOf()` and `sema::canonical()` always return the final type.

`analyze()` always completes and leaves a partial model even when there are errors. The library has no LLVM dependency.
