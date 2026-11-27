# bob — Language & Implementation Design (draft 0.1)

Status: first draft for discussion. Items marked **[Proposed]** are recommendations; items marked **[Open]** need a decision.
Source material: `docs/bob-plan.txt`, `D:\code\scratch-lang` (grammar_ideas.txt, syntax_ideas.txt, tests/*.scratch, src/scratch_runtime.*).

---

## 1. Goals and non-goals

Goals
- Smalltalk-80 flavoured syntax: everything *reads* as a message send.
- Statically typed. No untyped/dynamic variables.
- The compiler resolves as many sends statically as it can; only the remainder goes through a runtime `msgSend` in the style of `objc_msgSend`.
- Practical for systems work, up to and including an OS kernel (freestanding profile).
- Memory safe by default: ARC (Swift-like), bounds checks, no null for non-optional refs, no raw pointers outside `unsafe`.
- First-class built-in `string`, `array`, `dictionary`, `set`; the allocator knows the type and size of every block.
- Correct string handling: UTF-8 internally, with separate byte, codepoint and grapheme views, and no implicit byte-buffer conversions.
- Compile-time execution: code can send messages at compile time.
- Reflection, gated by security levels.
- Toolchain: C++20, CMake, LLVM backend, our own runtime, no MSVC C runtime.

Guiding principle
- **Smalltalk elegance where possible, practicality always.** The test is whether an OS kernel *and its drivers* can be written in bob. When the elegant form and the practical form conflict, practical wins, and the elegant form is offered as sugar on top where it can be.
- What "drivers" demands of the language:
  - no hidden allocations or hidden dispatch on hot or interrupt paths;
  - exact, C-compatible layouts;
  - volatile MMIO access;
  - port I/O and inline asm;
  - interrupt-safe code (no ARC traffic or allocation inside an interrupt handler unless explicitly allowed);
  - control over memory placement (DMA-able, aligned, physically contiguous via allocator hooks);
  - deterministic cleanup.
- Any feature that costs at runtime has to make that cost visible (for example `--explain-dispatch`) and must be avoidable.

Non-goals (for now)
- A tracing GC, cycle collector or image-based Smalltalk environment.
- Smalltalk's "everything is a heap object". Primitives stay raw machine values.
- Dynamic typing or `doesNotUnderstand:`-driven programming as a primary style.

---

## 2. Lessons carried over from scratch-lang

| Area | scratch-lang state | Carry forward / change |
|---|---|---|
| Lexer/Parser | Hand-written recursive descent, ~5k lines | Keep the hand-written approach and rewrite it cleanly for the revised grammar |
| Grammar | `program`/`lib`/`module` + version, `namespace`, `class`/`record`/`extends`, `public`/`private` scope blocks, `@[flags]`, `msg` declarations, `[T]` return types, `#` comments, `#{ }#` block comments, `:=` | Keep almost all of it (see §3) |
| Keyword sends | `f add: 3 @num2: 5` | Drop the `@` and use Smalltalk-style keyword tokens (see §3.4) |
| Entry point | `extends <program> { @[static] msg main: args:array [int32] }` | Keep |
| Runtime | C API (`Runtime_*`), no CRT, kernel32/user32/shell32 only, own `memset`/`memcpy`, `_fltused`, `/GS-`, custom entry point | Keep the strategy. Remove `<functional>`/`<string>` from the runtime (they drag in the CRT/STL). Add `__chkstk`, `memmove`, `memcmp` |
| Type descriptors | `Runtime_TypeDescriptor` enum, duplicated in compiler and runtime | Generate one shared header for both sides |
| Containers | Type-erased arrays/hashtables/dictionaries keyed by type descriptor | Keep type-erased runtime containers. The compiler infers element types and passes stride/type info (§4.5) |
| Strings | `char`-based, byte indexed | Replace with the UTF-8 design (§7) |
| Compile-time model | `compiletime.h` has Class/Record/Message/ScopedEnvironment | Re-derive it as a typed semantic model, built to be scriptable (§11.4) |

---

## 3. Syntax

The exact grammar the parser implements, including the disambiguation rules settled in phase 1 (contextual keywords, `;` optional before `}`, same-line rule for block receivers, bracketed parameter types), is in `docs/grammar.md`.

### 3.1 Compilation units (unchanged from scratch-lang)
```
program hello, 0.1.0          # executable
lib     collections, 2.3.0    # static/dynamic library
module  geometry, 1.4.11      # source module
```
A unit has a header, `import`s, then a `{ ... }` body with namespaces, classes, records, extensions and statements.

### 3.2 Lexical
- Comments: `# to end of line`, `#{ ... }#` block.
- Statement terminator: `;`. (Smalltalk uses `.`, but bob uses `.` for qualification `core.io.out` and field access `p.x`.)
- Assignment: `:=`. Equality: `=`. **[Decided]** Not equal is Pascal's `<>`. Shifts are Pascal's word operators `shl` and `shr` (`x shl 2`), reserved words that bind like `*`. There are no `<<`, `>>`, `==`, `~=` or `??` operators.
- Literals: `123`, `0x7F`, `0b1010`, `1_000_000`, `3.25`, `1e-9`, `"utf-8 string"`, `'é'` (unicode scalar / `char`), `true`, `false`, `nil`, array `[1, 2, 3]`.
- Suffix-typed literals **[Proposed]**: `10u8`, `3.0r32`. Otherwise literals are polymorphic and take their type from context, defaulting to `int32`/`real64`.

### 3.3 Declarations
```
x: int32;                 # explicit declaration (zero-initialised)
y: int32 := 4;            # explicit declaration + init
z := 4;                   # implicit declaration, type inferred (int32)
z := z + 1;               # assignment
```
A statement that begins with `identifier ':'` is an explicit declaration. Expressions never start with a keyword token, so this is unambiguous.

**[Decided]** Assigning to a name that isn't in scope declares it, and its type is inferred from the right-hand side. The variable is still statically typed: once inferred, its type is fixed (`z := "hi";` afterwards is a type error).

Rules that stop typos from silently creating variables:
- The right-hand side can't use the variable being declared (`cuont := cuont + 1;` is "undeclared identifier `cuont`").
- An implicit declaration can't shadow a field, parameter or outer local. That's an error, and you use an explicit declaration if you really mean it.
- A variable that is assigned but never read is a warning (an error under `--strict`). This catches `cuont := 5;`.
- The inferred type must be unambiguous. Untyped literals take their default (`int32`, `real64`), and `nil` alone can't be inferred (`p: Foo? := nil;`).
- Fields, parameters and return types are always written explicitly. Inference is for locals only.

### 3.4 Message sends
Smalltalk precedence: unary > binary > keyword, with parentheses for grouping.
```
c count;                              # unary
a + b;                                # binary
d at: 3;                              # keyword, selector at:
d at: 3 put: v;                       # keyword, selector at:put:
f add: 3 num2: 5;                     # selector add:num2:
x := (p distanceTo: q) sqrt;
```
- **[Decided]** Unary sends bind tightest and apply only to the nearest operand, as in Smalltalk and like method calls in C-family languages. `4 + 5 toString` means `4 + (5 toString)`, so you write `(4 + 5) toString + " yes"`. `i < items count` means `i < (items count)`. There's no low-precedence "then send" operator.
- **[Decided]** A keyword token is an identifier *immediately* followed by `:` (no whitespace), as in Smalltalk. There's no `@` prefix on later keyword parts.
- Every send has an explicit receiver (`self foo: 1`). A statement can't start with a bare keyword, which keeps §3.3 unambiguous.
- **[Decided]** Binary operators use conventional precedence instead of Smalltalk's strict left-to-right order, so `2 + 3 * 4` = 14. The overall order is still unary > binary > keyword. Binary precedence, highest first, is C's ordering *except* that the bitwise operators bind tighter than comparisons. That fixes C's well-known `x & mask == 0` trap, and Go, Swift and Rust do the same:

  | Level | Operators |
  |---|---|
  | 1 | `*` `/` `%` `&` `shl` `shr` |
  | 2 | `+` `-` `|` `^` |
  | 3 | `=` `<>` `<` `<=` `>` `>=` |
  | 4 | `and` |
  | 5 | `or` |

  All levels are left-associative. User-defined binary selectors (for example `+` on `Point`) take the precedence of their operator. Binary selectors that aren't in the table bind at level 3.
- **[Decided]** No cascades for now (Smalltalk's `;` is our statement terminator). Repeat the receiver instead. A Dart-style `..` cascade could be added later without breaking existing code.

### 3.5 Control statements and blocks
**[Decided]** Control flow uses formal statements, not Smalltalk-style messages to blocks. The message forms were already compiler special cases in disguise, they forced awkward grammar rules, they had no natural `break`/`continue`, and they made `return` inside a branch a subtle non-local return. Practicality wins (§1).
```
if a > b {
    max := a;
} else if a = b {
    max := 0;
} else {
    max := b;
}

while i < 10 {
    i := i + 1;
}

for i in 1 to: 10 {            # intervals
    sum := sum + i;
}
for each in items { ... }      # arrays, sets, dictionary keys, string views...
for c in Color { ... }         # enums and subranges
for b: uint8 in bytes { ... }  # optional explicit type

loop {                         # infinite loop
    if done {
        break;
    }
    if skip {
        continue;
    }
}

case c of {                    # Pascal case, see 4.6
    Color red, Color green: { self go; }
    Color blue:             { self wait; }
} else {
    self stop;
}
```
- **Braces are always required**, even for a single statement, for every `if`/`else`/`while`/`for`/`loop` body and every `case` entry. There are no one-line forms. `else if` chains are allowed, and each branch has its own braces.
- Conditions are ordinary expressions with no surrounding parentheses (like Swift, Go and Rust). A block literal can't appear unparenthesised in a condition, because its `{` would be ambiguous with the body: write `if (items anySatisfy: { :e | e }) { … }`.
- `break` and `continue` apply to the innermost `while`/`for`/`loop`. Labelled breaks may come later.
- `for x in e` works on anything iterable (arrays, sets, dictionaries, strings' views, intervals `a to: b`, enum and subrange types) and compiles to a direct loop.
- Reserved words: `if else while for in loop break continue case`. `of` is recognised only after a `case` subject, so `array of: Part` still works.
- `return` inside a control statement returns from the enclosing message, as you'd expect.

**What stays as messages.**
- **Blocks as values**: `{ :p1 :p2 | statements }`, with parameter types inferred from the expected block type or written explicitly (`{ :i: int32 | … }`).
- **Block-taking library messages**: `items do: { :e | … }`, `collect:`, `select:`, `1 to: 10 do: { … }`. These are ordinary messages any class can implement. For built-in collections with a literal block, the compiler inlines them.
- **Value-producing conditionals**: `m := (a > b) ifTrue: { a } ifFalse: { b };` is an ordinary message on `bool`.
- **User-defined control abstractions**: error blocks (`onError:`), thread blocks (`fork`) and the like (§5.6).
- `whileTrue:` and `caseOf:` are no longer part of the language. `while` and `case` replace them.
- `return expr;` inside a *block* returns from the enclosing message (Smalltalk `^` semantics). That's only allowed for non-escaping blocks.

### 3.6 Classes, records, extensions
```
class Man {
}

class Bob inherits Man implements Printable {
    public {
        x: int32;
        y: int32;
    }
    private {
        name: string;
    }

    msg toString: t:int32 isFat:bool [string] {      # selector toString:isFat:
        return "my name is " + name;
    }

    @[static] msg named: n:string [Bob] {            # class-side message
        b: Bob := Bob new;
        b.name := n;      # field access; allowed only inside the class
        return b;
    }
}

record Point {
    x: uint32;
    y: uint32;
}

extends Point {
    msg + other:Point [Point] {
        return Point x: x + other.x y: y + other.y;
    }
}
```
- Message declaration keeps the scratch-lang/plan form: `msg firstKeyword: p1:T1 kw2:T2 [Ret]`. The first keyword names the message; later parameter names *are* the keyword labels. Selector = `firstKeyword:kw2:`.
  **[Decided]** A later parameter can optionally rename itself with `label: name:Type`. The label goes into the selector, and the body uses the name:
  ```
  msg at: i:int64 put: value:T [T] { ... }      # selector at:put:, body uses i and value
  msg toString: t:int32 isFat:bool [string]     # compact form, still valid
  ```
  Parsing: after a keyword token in a message declaration, a second keyword token (`value:`) means "rename". A type name means "compact". Parameter names must be unique within a declaration.
- Return type: `[T]`. Multiple returns: `[int32, string]`. Named: `[count: int32, name: string]`.
- `#p.x := 100;` from the plan becomes `p.x := 100;`. Records allow direct field access. Classes only allow it from inside the class (outside, you go through accessor messages, which the compiler synthesises and inlines).

### 3.7 Attributes
`@[flag, flag: value]` before a declaration, as in scratch-lang. Initial set: `static`, `compiletime`, `inline`, `final`, `protocol`, `alias`, `escaping`, `volatile`, `packed`, `align: n`, `reflect: level`, `overflow: mode`, `extern: "C"`, `unsafe`.
Attributes can attach to declarations (module, class, record, message, field, variable), to a `{ }` statement block, or to a parenthesised expression `@[...] ( expr )`. Where several apply, the innermost wins.

### 3.8 Example program
```
program hello, 0.1.0
import core.io;
{
    class Counter {
        private { count: int64; }
        msg increment { count := count + 1; }
        msg count [int64] { return count; }
    }

    extends hello {
        @[static] msg main: args:array [int32] {
            c: Counter := Counter new;
            for i in 1 to: 10 {
                c increment;
            }
            if c count > 5 {
                io out println: "big";
            }
            return 0;
        }
    }
}
```

---

## 4. Type system

### 4.1 Primitives
`bool`, `int8 … int128`, `uint8 … uint128`, `real32`, `real64`, `char` (Unicode scalar value, 32-bit, always valid), `byte` (= `uint8`), `intptr`/`uintptr` (pointer-sized).
- Same layout as C. They are not objects and have no header, but they accept messages: `3 + 4`, `x abs`, `x max: y`, `n printString`.
- Messages on primitives are *intrinsics* (inline LLVM instructions) or direct calls into the runtime. They never go through dynamic dispatch.
- **[Decided]** Integer overflow traps by default (Swift model). The compiler removes the check wherever it can prove no overflow is possible.
  - Explicit messages, always available whatever the mode: `a wrapAdd: b` (two's complement), `a satAdd: b` (clamp), `a checkedAdd: b` (returns `T?`, `nil` on overflow). The same exists for sub/mul.
  - `@[overflow: trap | wrap | saturate]` changes how `+ - *`, `negated` and `shl` behave. It can attach to a module, a class or record, a message, a `{ }` statement block, or a single parenthesised expression. The innermost attribute wins.
    ```
    @[overflow: wrap] module kernel.mm, 0.1.0 { ... }
    h := @[overflow: wrap] (h * 31 + c);
    @[overflow: trap] msg allocSize: n:uint64 [uint64] { return n * 16; }
    ```
  - Division by zero always traps, in every mode. Signed `MIN / -1` traps under `trap`, gives `MIN` under `wrap`, and gives `MAX` under `saturate`.
- No implicit narrowing. Widening int→int is implicit only when lossless. int↔real conversion is always explicit (`x asReal64`).

### 4.2 Records (value types)
- Plain data with C-compatible layout (`@[packed]`, `@[align:]` available). Stack or inline storage. No header, no refcount.
- Fields are laid out exactly in declaration order (C rules), so a record can match any C struct.
- **[Decided]** There are two ways to reuse a record, and both are allowed:
  1. **Inheritance, as a layout prefix.** `record B inherits A` places all of A's fields first, then B's own fields. B gets A's fields and messages (`p.kind`). Messages on records are always static: there's no overriding and no vtable.
     ```
     record PacketHeader { kind: uint16; len: uint16; }
     record TcpPacket inherits PacketHeader {
         srcPort: uint16;
         dstPort: uint16;
     }
     # layout: [kind, len, srcPort, dstPort]
     p.kind := 6;
     ```
     A `B` converts to an `A` only by reference, where an `A` reference or pointer is expected. A by-value conversion would silently drop B's fields (C++ slicing), so it has to be explicit: `p asBase`.
  2. **Embedding, for any layout position.** When the C layout puts the shared part somewhere other than first, embed it as a field:
     ```
     record TcpPacketTrailer {
         srcPort: uint16;
         dstPort: uint16;
         hdr: PacketHeader;      # wherever the C layout requires
     }
     p.hdr.kind := 6;
     ```
     Later option (not committed): Go-style promoted access, so `p.kind` resolves to `p.hdr.kind` when it's unambiguous. Pure syntax sugar that doesn't affect layout.
- Messages on records (via `extends`) are always statically dispatched.

### 4.3 Classes (reference types)
- Heap objects with a header (§6.2). Single inheritance (`inherits`), plus any number of `@[protocol]` classes (`implements`).
- **[Decided]** A concrete class can only be subclassed inside its own module. That gives the compiler complete class-hierarchy knowledge, which is what makes most sends static (§5). Extension across modules goes through protocols (§4.4). See §5.4 for the full rationale.
- Non-optional class references are never `nil`. Optional: `Foo?`. `nil` is only assignable to optionals. Unwrap with messages: `x ifNotNil: { :v | ... }`, `x ifNil: { ... }`, `x ifNil: default`.

### 4.4 Protocols: `@[protocol] class` **[Decided]**
There's no separate `protocol` keyword. A protocol is a class marked `@[protocol]`:
```
@[protocol] class Shape {
    msg area [real64];                      # no body: required, every implementer provides it
    msg describe [string] {                 # body: default implementation
        return "shape with area " + self area printString;
    }
}

class Circle implements Shape, Printable {  # any module may do this
    private { r: real64; }
    msg area [real64] { return 3.14159 * r * r; }
}
```
Rules:
- No fields: a `@[protocol]` class can't have `public`/`private` blocks, and it can't be instantiated with `new`. Because there's no state, implementing several protocols has none of the diamond or layout problems of multiple inheritance.
- A message declared with `;` instead of a body is *required*. A message with a body is a *default* that implementers may override. Default bodies can only use `self` sends, since there are no fields.
- Classes conform with `implements` (any number, from any module). They can also conform retroactively through an `extends` block in another module (`extends Counter implements Printable { ... }`).
- A `@[protocol]` class can `inherits` another `@[protocol]` class to refine it. It can't inherit a concrete class.
- A value whose static type is a protocol is a reference to a conforming class instance. Sends through it are Tier 2 (`bob_msgSend`, §5.3). When the concrete type is known, sends stay static.
- **[Decided]** Records can't implement protocols. Records are strictly for data (§4.2).

### 4.5 Built-in first-class types
`string`, `array`, `dictionary`, `hashtable`, `set`, `slice` (bounds-checked view), tuples, blocks.

**[Decided]** Container element types are inferred, not written. There are no angle brackets in bob source.
| Type | Keys | Values / elements |
|---|---|---|
| `array` | integer index | inferred |
| `dictionary` | always `string` | inferred |
| `hashtable` | inferred | inferred |
| `set` | — | inferred (stored as a bitmask when the element type is an enum, §4.6) |

Inference is still fully static. Every container has one concrete element type, known at compile time:
```
a := [1, 2, 3];              # local: from the literal → int32
b: array;                    # local: from first use
b add: "hi";                 #   → string

msg sum: nums:array [int64] {           # parameter: the message is specialised per element
    total := 0;                          # type used at its call sites (checked per use)
    nums do: { :n | total := total + n; };
    return total;
}

msg names [array] { ... }               # return: from the return statements

class Inventory {
    private { items: array; }           # field: from the class's own messages
    msg add: p:Part { items add: p; }   #   → Part
}
```

**[Decided]** Homogeneous and mixed contents are both allowed, and the compiler chooses the representation:
- **One element type**: raw contiguous storage (`[1, 2, 3]` is 3 × 4 bytes). No tags, boxing or dispatch.
- **All classes that share a protocol**: the element type is that protocol, stored as references with protocol dispatch.
- **Mixed otherwise**: the element type is the *closed union* inferred from everything stored into the container in scope, e.g. `[7, 8, "oiudf", 1.5]` → `int32 | string | real64`. Each element is stored as a tag plus the value inline, so primitives aren't boxed.
  - A message every member understands (`(ff at: 2) printString`) compiles to a switch on the tag, with a direct call in each arm. No `msgSend`.
  - Anything else requires narrowing: `(ff at: 2) ifKindOf: int32 do: { :n | ... }`.
  - The same applies to `dictionary` and `hashtable` values (a JSON-object-like `dictionary` gets a `string | int32 | ...` value type), and to `hashtable` keys.
  - An accidental mix (`[1, 2, "3"]`) is usually caught at the first use that needs a specific type. The error message names the inferred union. `--explain-types` lists every inferred union.

**[Decided]** An optional annotation covers cases where inference can't decide (an array that's returned empty, a field only filled from outside the module). You write it only when the compiler asks for it, or to force a homogeneous container:
```
items: array of: Part;
d: dictionary of: int32;
h: hashtable keys: int32 values: Part;
```
- **[Decided]** There are no user-defined generics: no type parameters on classes, records or messages. The syntax and semantic complexity (C++ templates, Java's erased generics) isn't worth it. The cases generics usually cover are handled by:
  - built-in containers with inferred element types (above);
  - `Foo?` optionals;
  - protocols, for polymorphism over classes;
  - error blocks, instead of a `Result<T, E>` type (§5.6).
  Element-type specialisation (`msg sum: nums:array`) applies *only* to the built-in containers. It isn't a general mechanism.
  - **[Decided, revisit in phase 6]** For user-written containers, the planned answer is **type-returning compile-time messages** (Zig-style), not generic syntax. They are needed because kernels and drivers depend on custom containers: ring buffers, intrusive lists, slab pools, fixed-capacity arrays, and `Mutex`/`Atomic` wrappers that own the data they guard. The alternatives are duplicated code, protocols (dynamic dispatch, and impossible for records), or raw pointers (giving up memory safety).
    ```
    @[compiletime] msg RingBuffer of: T:type [type] {
        return class {
            private { items: array of: T; head: uint32; tail: uint32; }
            msg push: v:T { ... }
            msg pop [T?] { ... }
        };
    }
    q: RingBuffer of: Packet;     # an ordinary compile-time keyword send that yields a class
    ```
  - **Kept on a tight leash.** The explicit non-goal is the C++ failure mode, where templates grew into a second, accidental programming language. Rules (the starting point when this is revisited):
    1. A type-returning message's body is a single `return class { ... };` or `return record { ... };`. There's no compile-time logic around it: no branching or looping on `T`, and no `T meta` inspection to pick different layouts.
    2. Parameters are types. **[Open in phase 6]** whether compile-time integer constants are also allowed (`FixedArray of: Packet capacity: 64`). Subrange-indexed arrays (§4.7) already cover plain fixed-capacity buffers, so this is only needed for user-written containers.
    3. Requirements on `T` are expressed only through protocols (`T:type implements Comparable`) and checked where the generic is *defined*, so errors are reported there and not at each use.
    4. No specialisation, no partial specialisation and no overloading by type pattern. A failed instantiation is always an error, never a silent fallback (no SFINAE).
    5. Results are cached per argument set (`RingBuffer of: Packet` is one class program-wide), with a stable mangled name for linking, debugging and `meta typeName`.
    6. General compile-time code generation (`compiler` synthesising code, §8.1) stays a separate, explicitly-invoked tool. It isn't folded into this mechanism.

### 4.6 Enums
**[Decided]** Enums are named integer constants with an optional storage type, written like a declaration:
```
enum Color { red; green; blue; }                         # storage int32, values 0, 1, 2

enum PciClass : uint8 {                                  # explicit storage type
    storage := 0x01;
    network := 0x02;
    display := 0x03;
}

c := Color red;                    # an enumerator is a unary message to the enum type
k: PciClass := PciClass network;
```
- The default storage type is `int32` (matches C). Values count up from 0 or from the previous explicit value. Duplicate values are an error.
- Enumerators are always qualified (`Color red`). They don't leak into the surrounding namespace, so two enums can both define `red`.
- An enum is its own type, with **no implicit conversion** to or from integers:
  ```
  c value                    # storage-type integer
  Color fromValue: 2         # Color?, nil if not a member (the checked path for raw hardware bits)
  c name                     # "red"; the string is only emitted if used at runtime
  Color meta members         # compile-time list of members (§8.3)
  ```
  Equality and ordering (`=`, `<`) compare values within the same enum type only.
- No data attached to enumerators (Swift/Rust-style payloads). Sum types are covered by inferred unions (§4.5).

**[Decided] Pascal-style ordinal features.** Enums are *ordinal types*, a finite, ordered set of members, and the language builds on that:

| Feature | bob | Notes |
|---|---|---|
| Position | `c ordinal` | 0 … `count - 1`. Differs from `value` when explicit values are sparse |
| Next / previous | `c succ`, `c pred` | Stepping past `last`/`first` traps (consistent with overflow, §4.1) |
| Bounds | `Color first`, `Color last`, `Color count` | Compile-time constants |
| Iteration | `for c in Color { ... }`, `for c in Color red to: Color blue { ... }`; also `Color do: { :c \| ... };` | Direct loop |
| Sets | `s: set of: Color := [Color red, Color blue];` | See below |
| Enum-indexed arrays | `names: array of: string by: Color;`, `names at: Color red` | See below |
| Branching | `case x of { ... } else { ... }` with Pascal-style labels | See below |

**Sets of enums replace flags.** There's no `@[flags]` attribute. A `set` whose element type is an enum is always stored as a bitmask, with bit *n* = the member whose value is *n*. The storage width is the smallest of 8/16/32/64 bits that holds the largest value, or the enum's storage type if that's wider, so it's C-compatible.
```
enum Perm : uint32 { read; write; exec; }           # values 0, 1, 2 = bit positions
p: set of: Perm := [Perm read, Perm write];          # mask 0b011
if p includes: Perm write {
    ...
}
p := p + [Perm exec];                                # union
p := p - [Perm write];                               # difference
q := p * other;                                      # intersection
reg := p value;                                      # raw mask for a register
p := (set of: Perm) fromValue: reg;                  # checked: unknown bits → nil
```
Porting a C header means writing bit *positions* (`exec := 2;`), not masks (`0x4`).

**Enum-indexed arrays.** `array of: T by: SomeEnum` has exactly `SomeEnum count` elements, stored inline (fixed size, no heap), and `at:`/`at:put:` take the enum. Indexing can't go out of range, so there's no bounds check. Typical use is per-state and per-opcode tables in drivers:
```
handlers: array of: Handler by: Opcode;
(handlers at: op) run;
```

**`case` statement (Pascal `case`).** **[Decided]** This is the general branch for enums, integers and inferred unions. It's a statement, `case <subject> of { entries } [else { … }]`, always compiled to a jump table or compare chain. Each entry is a *label list, then `:`, then a braced body*. Labels can be members, constants, or ranges (`(1 to: 9)`). Without `else`, every member must be covered (a compile error otherwise, so adding a member later finds every `case` that needs updating). Labels must not overlap.
```
case c of {
    Color red, Color green: { self go; }
    Color blue:             { self wait; }
}

case n of {
    0:         { ... }
    (1 to: 9): { ... }
} else {
    ...
}

case v of {                          # inferred union int32 | string
    int32:  { :n | total := total + n; }
    string: { :s | names add: s; }
}
```
For a union subject, an entry body can bind the narrowed value as a block parameter (`{ :n | … }`). Parsing: the lexer's keyword token on the last label (`green:`) is split back into the label and the colon. That's unambiguous, because a case entry can't be an ordinary statement.

### 4.7 Subranges and named types **[Decided]**
A subrange is a contiguous slice of an integer or enum type, checked when values are assigned into it (Pascal).
```
type Percent := 0 to: 100;                  # storage = smallest int that fits (uint8)
type Port : uint16 := 1 to: 65535;          # explicit storage type, same `:` as enums
type Weekday := Day mon to: Day fri;        # enum subrange

pct: 0 to: 100;                             # inline in a type position, unnamed
sector: array of: uint8 by: [0 to: 511];    # fixed-size, index type 0..511
```
- `type Name := <type>;` names a type. `lo to: hi` in a type position is a compile-time message yielding a subrange type. Bounds must be compile-time constants.
- **Out is free.** A subrange value converts implicitly to its base type (always valid).
- **In is checked.** Constants are checked at compile time (`pct := 150;` is an error). Values proven in range need no check. Everything else traps at runtime (consistent with overflow, §4.1). `Percent fromValue: x` returns `Percent?` for a non-trapping check.
- **Arithmetic is done in the base type.** `pct + 1` is `uint8`, with the normal overflow rules. Assigning back into a `Percent` is checked. There's no range-arithmetic type algebra (unlike Ada).
- **Inference keeps the subrange.** `x := pct;` gives a `Percent`. `x := pct + 1;` gives a `uint8`.
- **Arrays indexed by a subrange** (`array of: T by: [lo to: hi]`) are fixed-size and stored inline. `at:` takes the subrange type, so there's no bounds check. This covers most fixed-capacity kernel buffers.
- **Enum subranges** support the ordinal messages (`succ`, `first`, `last`, `do:`). `case` on a subrange only has to cover the range.
- `meta` exposes `first`, `last` and `size`.
- **Out of scope:** non-contiguous ranges, `real` subranges, runtime-computed bounds.

**[Decided] Grouping in type positions uses `[ ]` only.** With one entry it groups, and with several it forms a tuple. Parentheses aren't allowed in type positions (`by: (0 to: 511)` is an error suggesting `[0 to: 511]`), so types and values look different at a glance:
```
grid: array of: [array of: real32 by: [0 to: 7]] by: [0 to: 7];
msg pos [int32, int32] { ... }       # tuple
msg ok [bool] { ... }                # single entry = just bool
```
In expressions, `[ ... ]` is still an array literal: `r := [0 to: 511];` is an array holding one interval.

**[Decided] Named types are distinct by default; `@[alias]` opts out.**
```
type PhysAddr := uint64;            # distinct
type VirtAddr := uint64;            # distinct
@[alias] type Handle := uint64;     # alias, interchangeable with uint64 (C-header translation)

self mapPage: virt to: phys;        # swapping VirtAddr and PhysAddr arguments is a compile error
```
- A distinct type has the same storage, operators and messages as its base. Operators work between values of the *same* distinct type and give that type (`p + q` is a `PhysAddr`). Untyped literals adopt the type (`phys + 4096`).
- Mixing with the base type or another distinct type needs an explicit conversion: `p value` (to the base) and `PhysAddr fromValue: n`. `fromValue:` returns `T` when every base value is valid (plain distinct types) and `T?` when not (enums, subranges).
- `extends PhysAddr { ... }` can add messages to a distinct type (statically dispatched, like primitives). Aliases can't be extended separately, because they *are* the base type.
- Subranges are the exception to "distinct": a subrange still converts implicitly *out* to its base (see above), because it's a checked subset of the base, not a separate unit of meaning. `type` of a record or class makes a distinct type with the same layout or behaviour.

---

## 5. Message resolution and dispatch

The central idea: syntactically every operation is a send, and semantically the compiler picks the cheapest correct implementation.

### 5.1 Resolution tiers (decided per send site, at compile time)
| Tier | When | Lowered to |
|---|---|---|
| 0 Intrinsic | Receiver is a primitive, or a built-in with an intrinsic selector (`array at:`, `bool ifTrue:ifFalse:` and collection `do:` with literal blocks) | Inline LLVM IR (`add nsw`, `icmp`, branch, bounds-checked GEP) |
| 1 Static | Receiver static type is a record; or class message (`@[static]`); or selector is `@[final]`; or class-hierarchy analysis shows exactly one implementation reachable in the module's closed hierarchy; or the exact dynamic type is known (just constructed, flow analysis) | Direct call, candidate for inlining |
| 1v Vtable | Receiver static type is a class and several overrides are reachable in its closed hierarchy (§5.4) | One indexed load from the class's vtable + indirect call |
| 2 Dynamic | Receiver static type is a `@[protocol]` class; reflection `perform:`; dynamically loaded library | `bob_msgSend(self, sel, args...)` |

The compiler emits a diagnostic note option (`--explain-dispatch`) that reports which tier each send got. That's how you can confirm the "resolve everything it can" goal is being met.

Type checking always happens. A send is a compile error if the static type can't respond to the selector, even when the send will be dynamically dispatched. Tier 2 changes *how* we dispatch, not *whether* it's valid.

### 5.2 Runtime objects for dispatch
- **Selector (SEL)**: pointer to a uniqued, interned selector record (`"toString:isFat:"` + hash). The runtime uniques selectors at image/library load. Within a statically linked program, uniquing happens at link time via COMDAT sections.
- **Class object**: `isa` (metaclass), `super`, name, instance size, ivar layout (for ARC and reflection), method list (`SEL → IMP + type signature`), protocol list, per-class method cache, reflection metadata and reflection level.
- **Object header** first word is the class pointer, so `msgSend` needs only one load to find the cache.

### 5.3 `bob_msgSend`
Modelled on `objc_msgSend` (see the zhengdong.me article):
1. `self` in RCX, `sel` in RDX (Win64 ABI). Other args stay untouched in registers/stack.
2. Load `isa` from `[self]`, then the cache from `isa`.
3. Hash `sel & mask`, probe the open-addressed cache. On a hit, `jmp imp` (tail jump, so arguments pass through unmodified).
4. On a miss, save argument registers and call the C++ slow path: walk the method lists up the superclass chain, fill the cache, restore registers, `jmp imp`.
5. If no method is found: call `doesNotUnderstand:` if defined, else a runtime fatal error. This can only happen through reflection or stale dynamic libraries, because static typing rules it out otherwise.

Written in MASM (`src/runtime/win64/msgSend.asm`, CMake `enable_language(ASM_MASM)`). scratch-lang already has an `.asm` file for memcpy. Variants: `bob_msgSend` and `bob_msgSendSuper`. Struct returns go through a hidden pointer arg on Win64, so no `_stret` variant is needed.

### 5.4 Closed class hierarchies **[Decided]**

**Problem.** For `s area` where `s: Shape`, the compiler can only make a direct call if it knows *every* class that could be behind `s` and which `area` each one uses. If any module compiled later, or any DLL, can subclass `Shape` and override `area`, that knowledge is never complete. Every send of an overridable message through a class-typed reference then has to be dynamic. Java's JIT works around this at runtime by speculating and deoptimising. We're ahead-of-time compiled and have no such safety net.

**Rule.** A concrete class can only be subclassed (`inherits`) by classes in the same module, with no exceptions and no `@[open]` escape hatch. Extension from outside a module always goes through a `@[protocol]` class (§4.4). Inheritance is a code-reuse tool inside one module, and protocols are the extension point between modules.

`@[final]` on a class (no subclasses at all) or on a message (never overridden) is still allowed. It's an optimisation hint and a statement of intent inside the module. The compiler can usually work it out on its own.

**What the compiler does with a closed hierarchy.** When compiling module `geometry`, it computes for each class and selector the set of implementations reachable from that static type:
- **One implementation** (the common case, for example an `area` that nobody overrides): a direct call, usable for inlining. Tier 1.
- **Several implementations**: the hierarchy is fixed, so the compiler can safely lay out a per-class **vtable** and dispatch with one indexed load. A small set can also become a type-check-and-branch on the class pointer. Vtables are normally *fragile* across modules: adding a method in a base class shifts every subclass's slots, which is C++'s fragile base class problem. Here no one outside the module can subclass, so the module always controls every slot.
- The hierarchy facts are written into the module's interface file. An importing module that sends `area` to a `Shape` reads "closed; implementations = {Shape>>area, Circle>>area}" and devirtualises the same way.

**What remains dynamic.** Only three sources: sends through `@[protocol]`-typed values, reflection `perform:`, and dynamically loaded code. Those use `bob_msgSend`. A send through a *class* type is never dynamic.

**Costs.**
- You can't subclass another module's *concrete* class, for example a library base class with fields that users extend (UIKit `UIViewController` style). The workaround is a protocol plus composition or delegation, which Go and Rust force anyway. If this ever proves necessary, an `@[open]` attribute can be added later as a purely additive change.
- Adding a subclass inside a module changes that module's interface (the implementation sets), so dependents must recompile. That's fine for source modules. Libraries have to bump their version (modules and versioning, §12 phase 7).
- `extends` blocks in other modules can still *add* new messages to a closed class. Added messages are statically dispatched and can't be overridden, so they don't break the analysis.

Prior art: Swift (`open`/`public`/`final`), Kotlin (final by default), C# (`sealed`) and Java 17 sealed classes all restrict subclassing. Go and Rust drop implementation inheritance entirely and extend through interfaces/traits. bob sits between them: inheritance inside a module, protocols across modules.

### 5.5 Blocks / closures
- Non-escaping literal blocks (the vast majority) are inlined or lowered to a local function plus a frame pointer, with zero allocation.
- Escaping blocks are ARC-managed closure objects (function pointer + captured context). Escape is inferred from the parameter type: `@[escaping]` on block parameters, Swift-style.

### 5.6 Blocks as a unifying concept **[Notes, to be designed]**
Captured ideas, not yet decisions. The guiding idea: **a block is anything in `{ ... }`**, and blocks are used for many purposes. scratch-lang already modelled some of these (`PreConditionBlock`, `PostConditionBlock` and `ThreadBlock` in `compiletime.h`).

1. **Code blocks.** Message bodies, statement groups, control-statement bodies, and block arguments to messages such as `do:` (§3.5).
2. **Message objects (closures).** A block is a first-class value: a lambda, closure or function pointer. It can be stored, passed and returned ("message blocks can be passed around"). Non-escaping blocks are free. Escaping ones are ARC closure objects (§5.5). To design:
   - the block type syntax (with no angle brackets, so probably inferred like containers);
   - whether a block can be taken from an existing message (`obj messageFor: "add:"` → block bound to `obj`);
   - whether a block with no captures is guaranteed to be a plain C function pointer, which matters for callbacks and interrupt vectors.
3. **Pre- and post-condition blocks.** Optional per message, design-by-contract style. Sketch only:
   ```
   msg pop [int32]
       pre  { items isEmpty not }
       post { :result | items count = (old items count) - 1 }
   {
       return items removeLast;
   }
   ```
   To design:
   - the exact syntax;
   - `old` values in post-conditions;
   - when checks run (always, debug only, or per `@[contracts: ...]` level);
   - proving conditions at compile time and dropping the check, in line with "resolve at compile time";
   - whether protocol messages can declare contracts that implementers inherit.
4. **Error-trapping blocks: a safer alternative to exceptions.** The goal is error handling without exception tables or stack unwinding, which are hard to make safe in kernels, drivers and across C boundaries. Direction to explore:
   - Errors are values that travel back through **ordinary returns**, the way Swift's `throws` is implemented or Rust's `Result` with `?`. ARC cleanup then happens at normal scope exits and nothing unwinds the stack.
   - An error block traps them:
     ```
     { f := File open: path; data := f readAll; }
         onError: { :e | log warn: e message; };
     ```
   - The compiler knows which messages can fail, so a send that can fail outside a handling block is a compile error unless it's explicitly propagated.
   - Open: how propagation is written; the error value type (a protocol? a union of error records?); and how hardware traps (overflow, bounds, divide-by-zero) relate. They're fatal by default, but a kernel may want to trap them in a block, for example to isolate a faulty driver.
5. **Thread blocks.** Blocks as the unit of concurrency, possibly combined with error blocks. Sketch:
   ```
   t := { self crunch: chunk } fork;            # runs on another thread
   r := t join;                                 # result, or the error it trapped
   { ... } forkAll: workers onError: { :e | ... };
   ```
   To explore:
   - structured concurrency (threads started in a block must finish before the block exits);
   - rules for what a thread block may capture (immutable values, or atomically refcounted objects only);
   - how ARC atomicity (§6.1) interacts;
   - kernel-mode equivalents (work items, deferred procedure calls).

---

## 6. Memory management

### 6.1 Model **[Proposed]**
- Swift-style ARC for class instances and the storage of built-in types. The compiler inserts `retain`/`release`. Parameters are passed +0 (borrowed) by default, which removes most traffic. Returns are +1.
- Locals live in stack slots. Scope exit releases. "Stack-based reference counting": objects proven not to escape (escape analysis) are allocated *on the stack* with no refcount traffic at all.
- `weak` (zeroing, yields an optional) and `unowned` reference modifiers break cycles. No cycle collector.
- Value semantics with copy-on-write for `string`, `array`, `dictionary`, `set`. Assignment copies the reference and bumps the count, and mutation copies only if the count is > 1.
- Refcounts are atomic by default (correct across threads). Later optimisation: non-atomic for objects proven thread-local.

### 6.2 Tracked allocator
Every block the allocator hands out carries a header:
```
struct BlockHeader {        // 16 bytes, 16-byte aligned payload follows
    TypeInfo* type;         // class object or built-in type info
    uint32    refCount;
    uint32    flags;        // heap/stack, immutable, has-weak-refs, ...
};
```
Size comes from the allocator's size class (small blocks) or a large-block record. This enables:
- `sizeOf`/type queries on any live block (reflection, debugger, leak reports; scratch-lang already reports leftover bytes at exit),
- validation in debug builds (header magic, double-free detection),
- typed deallocation (the destructor is found from `type`).

Allocator: start on `HeapAlloc` (as scratch-lang does), then replace it with a size-class allocator over `VirtualAlloc`. The platform layer exposes `pageAlloc`/`pageFree` so the freestanding profile can plug in a kernel page allocator.

### 6.3 Safety rules
- Bounds checks on all indexed access. The optimiser elides them when it can prove the index is in range.
- No pointer arithmetic, raw pointers or reinterpretation outside `@[unsafe]` blocks/messages.
- No use-after-free in safe code (ARC + no raw pointers).
- Uninitialised reads are impossible (definite-initialisation analysis, or zero init).

### 6.4 Systems / OS profile
- `unsafe` gives typed pointers (`p: pointer to: uint32;`, same annotation style as `array of:`), `address`, reinterpret, `@[volatile]` loads/stores, inline asm (via LLVM inline asm), `@[packed]`/`@[align:]` records, `@[extern: "C"]` and `@[callconv: interrupt]`.
- **Freestanding profile** (`bobc --profile=freestanding`): no OS layer. The program supplies the page allocator and output hooks. The runtime core (ARC, containers, strings, msgSend) still works.

---

## 7. Strings

### 7.1 Representation
- `string` is a value type (COW, ARC-managed buffer) whose **invariant is valid UTF-8**. Every constructor validates or is statically known valid (literals are checked at compile time).
- Small-string optimisation (≤ 15 bytes inline) is a later optimisation.
- Literals live in read-only data with an "immortal" refcount flag: no allocation, no refcount traffic.

### 7.2 Views, no ambiguous length
`string` itself has no `size`, `length` or integer `at:`. You pick a view:
```
s bytes        # ByteView     : collection of uint8, read-only
s codepoints   # CodepointView: collection of char (Unicode scalars)
s graphemes    # GraphemeView : collection of string (extended grapheme clusters, UAX #29)

s bytes count; s codepoints count; s graphemes count;
```
- Indices are opaque per view (`string.index`), backed by byte offsets. You can't index with an int, except `s bytes at: n`, which is explicitly byte-oriented.
- Slicing (`s from: i to: j`) takes indices, so you can't split a codepoint.
- Grapheme segmentation and case mapping need Unicode tables. These are generated at build time from the UCD into compact tables in the runtime (a tool under `tools/unicodegen`).
- **[Decided]** Equality (`=`) and ordering (`<`) are byte-wise: a memory compare, which for UTF-8 is the same as codepoint order. Hashing is byte-wise too. Canonical equivalence is explicit: `a equalsCanonically: b`, `s normalized: NormalForm nfc`. Locale-aware collation is a library feature, not part of `<`.

### 7.3 Explicit, checked conversions only
```
s := string fromUtf8: bytes;          # [string?] or result type, fails on invalid input
s := string fromUtf8Lossy: bytes;     # replaces invalid sequences with U+FFFD
b := s toUtf8;                        # copies to an array of uint8
s := string fromUtf16: wideBuffer;    # Win32 interop
s withCString: { :p | ... };          # unsafe, NUL-terminated temporary
```
There are no implicit conversions between `string` and byte arrays or raw pointers. The runtime's Win32 layer converts command line and console IO between UTF-16 and UTF-8 (`GetCommandLineW`, `WriteConsoleW`/`WriteFile`).

---

## 8. Compile time execution

### 8.1 Semantics
- `@[compiletime]` on a message makes it callable only at compile time. On a statement or expression, it is evaluated during compilation and the result is embedded as a constant.
- Compile-time code is ordinary bob, type checked like everything else. It may only depend on declarations already compiled (the compiler orders declarations by dependency, and cycles are errors).
- It can send messages to a `compiler` object exposing the semantic model: query types, fields, selectors, sizes; emit diagnostics; synthesise messages, classes and conformances. This is the hook for generics, derive-style code generation (`printOn:`, equality, serialization) and static asserts.
```
@[compiletime] {
    if (Point meta size = 8) not {
        compiler error: "Point layout changed";
    }
}
TABLE := @[compiletime] (self crc32Table);     # array of uint32, embedded as constant data
```

### 8.3 Compile-time reflection: `meta` **[Decided]**
Every variable, field, parameter, expression and type can be queried at compile time, in the spirit of C++26 static reflection (P2996, `^^x` / `std::meta::info`). In bob it's a message, `meta`:
```
p: Point;
p meta typeName          # "Point"
p meta size              # 8         (bytes; = sizeof)
p meta alignment         # 4
p meta name              # "p"       (the variable's own name)
p meta type              # the Point type itself, as a compile-time value
Point meta fields        # [x, y]    (field infos: name, type, offset, size)
Point meta messages      # message infos: selector, parameter types, return type
ff meta type             # int32 | string | real64  (inferred unions are visible too)
ff meta elementType members   # [int32, string, real64]
```
Rules:
- `meta` is a reserved unary message. It is *always* evaluated at compile time, on any receiver: primitives, records, classes, unions, containers, and types themselves. It never conflicts with a user's own `size`/`name` messages, because those don't go through `meta`.
- The result is a compile-time constant. It can go anywhere a constant can: array sizes, `@[align: p meta size]`, conditions inside `@[compiletime]` code, and string literals.
- **Zero runtime cost.** No metadata goes into the binary unless code *uses* a `meta` result at runtime (for example storing `p meta typeName` in a variable embeds only that string). That's why it isn't gated by the runtime reflection levels of §9. The normal access rules still apply: `meta fields` outside a class lists only public fields.
- **Compile-time iteration and splicing** (the C++26 `template for` / `[: r :]` equivalent). A `@[compiletime]` loop over meta info is unrolled. A field or message info used as a send argument resolves statically, so derive-style code costs nothing:
  ```
  msg printOn: s:Stream {
      @[compiletime] (self meta type fields) do: { :f |
          s print: f name; s print: " = "; s print: (self fieldAt: f) printString;
      };
  }
  # unrolled to: s print: "x"; ... (self.x) printString; s print: "y"; ...
  ```
- `meta` and the `compiler` object (§8.1) share one underlying semantic-model API, which is also the one exposed to Python (§11.4).

### 8.2 Implementation **[Proposed]**
JIT the compile-time code with LLVM ORC in the compiler process, linked against a *hosted* build of the runtime. That's the same codegen as runtime code, so the semantics are guaranteed identical. An AST interpreter would be a second implementation of the language to keep in sync.
- Sandbox: compile-time code gets no filesystem/network access except through `compiler` (for example `compiler readFile:` relative to the module, recorded as a build dependency).
- Values crossing from compile time to runtime must be serialisable: primitives, records, strings, arrays, dictionaries of those. Class instances that are *not* pure data are an error.
- Cross-compilation: compile-time code runs for the host. Target-dependent queries (`pointerSize`) go through `compiler target`.

---

## 9. Reflection with security levels

Levels **[Proposed]**, each a superset of the one before:
| Level | Name | Grants |
|---|---|---|
| 0 | `none` | No metadata emitted beyond what dispatch needs |
| 1 | `identity` | `class`, `className`, `isKindOf:`, `respondsTo:` |
| 2 | `introspect` | Enumerate public messages, fields, protocols, signatures; read public fields |
| 3 | `invoke` | `perform:with:...`, write public fields, instantiate by name |
| 4 | `privileged` | Private fields and messages |

- *Emitted* level: per class via `@[reflect: introspect]`, with a module default, and a program-wide maximum set at build time. Metadata that isn't emitted doesn't exist in the binary, which matters for size and for attack surface.
- *Access* level: reflection is done through a `Reflector` capability object, not ambient globals. `main` receives (or can request) the root reflector and can hand out reduced ones (`reflector restrictedTo: ReflectLevel introspect`). Level 4 requires a reflector created with privileged rights.
- **[Decided]** For now, access is controlled by capabilities alone. A reflector can only be weakened when it's passed on, never strengthened. Planned for later: a module-header declaration (`@[reflect.requires: invoke]`) checked at compile and link/load time, so a program can see what a library needs and refuse to link it.
- Reflection is the main consumer of Tier-2 dispatch and the tracked allocator metadata.
- **[Decided]** There are no symbol literals. Selectors and names are passed to reflection as strings: `obj respondsTo: "toString:isFat:"`, `r perform: "count" on: obj`. When the argument is a string literal, the compiler validates it at compile time and pre-interns the selector, so it costs the same as a symbol. Non-literal strings are interned at runtime.

---

## 10. Runtime

### 10.1 Constraints (from scratch-lang)
- Not linked to the MSVC CRT: `/NODEFAULTLIB`, `/GS-`, no C++ exceptions or RTTI in the runtime, custom entry symbol (`bobStart`) that calls `Runtime_init` → `main:` → `Runtime_terminate` → `ExitProcess`.
- The runtime provides what LLVM/MSVC codegen may call implicitly: `memcpy`, `memmove`, `memset`, `memcmp`, `_fltused`, `__chkstk` (needed for frames > 4 KB on Windows, in asm), and later 128-bit int helpers (`__divti3`, etc.) and float math (`sqrt` intrinsic is fine; `pow`, `sin` and friends need our own implementations or a vendored libm subset).
- Imports only `kernel32`, listed in `kernel32.def` (§11.2). The runtime parses the command line itself (`GetCommandLineW` + its own splitter), so it doesn't need `shell32`'s `CommandLineToArgvW` the way scratch-lang did.

### 10.2 Structure
```
src/runtime/
  core/        object header, ARC, selector table, class registry, msgSend slow path
  memory/      tracked allocator
  containers/  array, dictionary (hashtable), set
  text/        string, UTF-8 codec, views, unicode tables (generated)
  reflect/     reflection API
  platform/    win64/ (console, memory, args, exit, msgSend.asm, chkstk.asm), freestanding/
  crt/         memcpy/memset/memmove/memcmp, _fltused
```
- Written in a freestanding subset of C++20 (templates, constexpr; no STL, exceptions or RTTI), exposing a C ABI (`bob_*` functions) to generated code.
- Type descriptors and runtime structs that the compiler must agree on live in one header, `src/shared/abi.h`, used by both compiler and runtime. That fixes scratch-lang's duplicated enum.

---

## 11. Compiler architecture

### 11.1 Pipeline
```
source → Lexer → Parser → AST
       → Sema (names, types, protocols, class hierarchy, definite init)
       ↔ CompileTime (ORC JIT; interleaved with Sema by dependency order)
       → Typed AST with resolved sends (tier + target per send)
       → [later] BIR: mid-level IR with explicit ARC ops, for ARC optimisation & devirtualisation
       → LLVM IR → LLVM opt → object file
       → lld-link (+ bobrt.lib, kernel32.lib) → .exe
```
**[Proposed]** Lower the typed AST straight to LLVM for the early phases. Design the typed AST's send annotations so a BIR (in the spirit of Swift SIL) can be inserted later, once ARC optimisation becomes the bottleneck.

### 11.2 Linking
Drive `lld-link` (ships with LLVM) instead of MSVC `link.exe`.
- **[Decided]** Import libraries are generated, not taken from the SDK. `src/runtime/platform/win64/kernel32.def` lists every OS function the runtime imports. At build time `llvm-dlltool` turns it into `kernel32.lib`, and `bobc` links against that. The `.def` file is the runtime's entire, reviewable dependency on Windows. The toolchain needs only LLVM + bob.
- **Fallback:** if a generated lib is missing or `llvm-dlltool` fails, `bobc` falls back to the Windows SDK import libs (found via `vswhere`/registry), or to a path given with `--sdk-lib-path`.
- User code that calls other Win32 APIs through `@[extern: "C"]` either adds its own `.def` file to the build (`bobc --import-def user32.def`) or links an SDK lib.

### 11.3 Diagnostics
Errors carry file/line/column ranges and notes. `--explain-dispatch`, `--dump-ast`, `--dump-llvm` flags on `bobc`. (Per CLAUDE.md, these are product features of the compiler, not test hooks. Tests live in `bobTests`.)

### 11.4 Scriptability (Python)
The semantic model (modules, types, classes, messages, send sites) is designed so it can be exposed to Python later (nanobind/pybind11) and to compile-time bob code through `compiler`:
- Nodes live in per-module arenas with stable integer IDs. Cross-references use IDs or handles, not raw owning pointers.
- Plain accessors, no deep template APIs on the public surface.
- One "query" API that both Python and `compiler` bind to.

### 11.4a Tooling **[Decided]**
The lexer, parser and AST are a separate library, `bobSyntax`, with no LLVM dependency, so tools for bob code (formatters, linters, language servers, doc generators, refactoring tools, Python bindings) can be built on the same front end as the compiler. The library is lossless (tokens plus trivia reproduce the source), error-tolerant (always a complete tree, with `Error` nodes), position-precise (node and name ranges), and generically walkable (`childrenOf` / `propertiesOf` / parent links / stable ids). It also has a JSON dump for tools in other languages. The contract is in `docs/tooling.md`. Later phases (semantic model, compile-time `compiler` object) follow the same rules.

### 11.5 Repository layout
As built through phase 1 (build instructions: `docs/building.md`):
```
CMakeLists.txt, CMakePresets.json
src/shared/abi.h          compiler/runtime shared symbol names & ids
src/syntax/               bobSyntax (no LLVM): source/, diagnostics/, lexer/, ast/, parser/
src/semantic/             bobSemantic (no LLVM): types, semantic model, analyzer
src/compiler/codegen/     AST + semantic model → LLVM IR
src/compiler/driver/      driver options, .bob pipeline, optimization + object emission, toolchain lookup, lld-link → bobCompiler
src/runtime/              bobrt: crt/ (mem*, _fltused), platform/win64/ (bobStart, console, chkstk.asm, kernel32.def)
src/tools/bobc/           bobc.exe (thin main over bobCompiler)
tests/                    bobTests: lexer, parser, ast, syntax corpus, semantic, driver, end-to-end, run programs (tests/programs/run, at -O0 and -O2)
```
Planned additions:
```
src/shared/abi.h          compiler/runtime shared layouts & type ids
src/compiler/             lexer, parser, ast, sema, ctfe, codegen, driver  → bobCompiler (static lib)
src/runtime/              → bobrt (static lib, freestanding flags)
src/tools/bobc/           bobc.exe (thin main over bobCompiler)
tools/unicodegen/         generates Unicode tables
lib/core/                 bob standard library sources (core.io, collections...)
tests/                    bobTests (GoogleTest): unit tests for lexer/parser/sema/codegen, plus
                          end-to-end tests that compile tests/programs/*.bob and run the exe
docs/
```
Runtime unit tests: gtest needs the CRT, so `bobTests` compiles the runtime sources in a *hosted* configuration (same sources, a different platform shim). End-to-end tests cover the real freestanding build.

---

## 12. Phased plan

| Phase | Deliverable | Exit criterion |
|---|---|---|
| 0 ✅ | CMake skeleton, LLVM found, gtest `bobTests`, `bobc` stub; `bobrt` freestanding with `bobStart`, console write, exit; link with lld-link | A hand-written LLVM IR "hello" links against bobrt with no CRT and runs. **Done 2026-11-27:** 16/16 tests pass in Debug and Release; exes import only kernel32 |
| 1 ✅ | Lexer + parser for §3 (all constructs), AST, `--dump-ast` | Parses all of scratch-lang's tests (converted) and the §3.8 example. **Done 2026-11-27:** `bobSyntax` library (lossless, error-tolerant, tool-reusable); 17 converted scratch tests + §3.8 example + a file of every §3–4 construct parse cleanly; 67/67 tests in Debug and Release |
| 2 ✅ | Sema + codegen for primitives, locals, records, intrinsic sends, `main:`, `return` | `factorial`/`fib` programs compile, run and return correct exit codes. **Done 2026-11-27:** `bobSemantic` (no LLVM) + LLVM codegen; also pulled in from phase 3: `if`/`while`/`for … to:`/`loop`/`break`/`continue`/integer `case`. Overflow modes, checked conversions and runtime traps (exit 101). 11 run programs at -O0 and -O2; 119/119 tests in Debug and Release. Rules as built: `docs/semantics.md` |
| 3 | Blocks: block values, non-escaping closures, inlined `do:`/`to:do:`/`ifTrue:ifFalse:`; `for` over collections once they exist; `--explain-dispatch`. (Control statements were done in phase 2.) | Block/closure programs; dispatch report shows tier 0/1 |
| 4 | Classes: layout, `new`, ARC insertion, static dispatch + CHA, class objects, selectors, `bob_msgSend` + cache, protocols | Polymorphism tests; leak report shows 0 bytes at exit |
| 5 | Built-ins: `string` (UTF-8, views, conversions), `array`, `dictionary`, `hashtable`, `set` with element-type inference and inferred unions, COW | String/collection test suite incl. grapheme cases |
| 6 | Compile-time execution via ORC; `compiler` object; `meta`; static asserts; constant tables. Revisit type-returning compile-time messages under the §4.5 leash rules | Compile-time CRC table and layout asserts work; decision made on `RingBuffer of: T` |
| 7 | Modules/libraries: interface files, imports, separate compilation, versioning | Multi-module program with a static lib |
| 8 | Reflection levels + Reflector capability; escaping closures; `weak`/`unowned` | Reflection tests per level, including denied access |
| 9 | Systems profile: `unsafe`, pointers, volatile, packed/align, inline asm, freestanding target | Boots a UEFI "hello" or a minimal kernel under QEMU |
| 10 | Python bindings over the semantic model | Script enumerates classes/sends in a module |

---

## 13. Open questions (summary)
Decided: `x := 4;` implicit declaration with inference (§3.3); `<>` for not equal and `shl`/`shr` word operators, no `<< >> == ~= ??` (§3.2); parameters and `for` variables are read-only, `and`/`or` short-circuit, runtime traps exit with code 101, program messages must be `@[static]` (docs/semantics.md); no `@` on keyword parts (§3.4); conventional operator precedence (§3.4); no symbol literals (§9); `@[protocol] class` replaces a `protocol` keyword (§4.4); concrete classes are closed to their module and there's no `@[open]` (§4.3, §5.4); unary binds tightest, no `->` (§3.4); no cascades (§3.4); compact message declarations with optional rename (§3.6); records allow both layout-prefix inheritance and embedding (§4.2); overflow traps by default, `@[overflow:]` at any level (§4.1); byte-wise string equality (§7.2); reflection access by capabilities, header declarations later (§9); generated import libs with SDK fallback (§11.2); inferred container element types, inferred unions for mixed contents, `array of: T` annotation (§4.5); compile-time reflection via `meta` (§8.3); records can't implement protocols (§4.4); no generic syntax; type-returning compile-time messages, tightly restricted, are the planned answer for user containers, revisited in phase 6 (§4.5); enums `enum Name : type { a := 1; }`, no implicit integer conversion, Pascal ordinal features, `set of: Enum` bitsets instead of flags, enum-indexed arrays, `case x of { … } else { … }` statement with Pascal labels (§4.6); formal control statements `if/else if/else`, `while`, `for … in`, `loop`, `break`, `continue`, braces always required, `whileTrue:`/`caseOf:` removed (§3.5); Pascal subranges, `type Name := ...`, `[ ]`-only grouping in type positions; named types are distinct by default, with `@[alias]` (§4.7).

Still open:
1. Can records have messages (statically dispatched helpers via `extends`, as in §3.6), or are they pure field layouts with no messages at all (§4.2)? Undecided, deferred. Leaning towards "no messages on records" (records strictly for data). The draft still shows record messages in §3.6 and §4.2 until this is decided.
2. Blocks as a unifying concept (§5.6): closures, pre/post conditions, error-trapping blocks, thread blocks. Ideas only so far.
