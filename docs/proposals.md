# bob — proposals: `@[noalloc]`, itable dispatch, tagged unions and errors

Status: draft for discussion, written against the phase 7 tree (`docs/design.md`, `docs/semantics.md`, `src/semantic`, `src/compiler/codegen`, `src/shared/objectModel.h`). Everything here is **[Proposed]** unless marked **[Open]**. Code references were read, not run: I couldn't build the backend, so each proposal lists what to verify first.

**Updated 2026-11-27 for phase 8 part 1.** Since this was written:
- escaping closures, boxed captures and `@[weak]`/`@[unowned]` were built;
- inferred unions were removed (a mixed literal needs a common type or a shared class or protocol);
- `case` entries no longer bind values;
- block types are written in braces (`{ int32, int32 [bool] }`);
- `&`, `|` and `^` were replaced by `and`, `or` and `xor`.

The sections below are corrected to match. **§2 is being built first**, before phase 8's reflection levels, because reflection (`perform:`, the Reflector) is what will keep `bob_msgSend` once protocol sends move to itables (§2.3).

| § | Proposal | Touches | Needs | Size |
|---|---|---|---|---|
| 1 | `@[noalloc]` | new analyzer pass, `.bobi`, small codegen "borrow mode" | nothing (protocol contracts are nicer after §2) | medium (about the size of `analyzerEscape.cpp`) |
| 2 | Itable dispatch for protocols | `objectModel.h`, class emission, one call site in codegen, runtime | nothing | small |
| 3.2 | Named tagged unions | types, layout, ARC glue, `case`, `.bobi` | nothing | large |
| 3.3 | Errors: `throws` / `try` / `fail` | parser, analyzer, codegen, `.bobi` | 3.2 | medium |

**Recommended build order: §2, §1, §3.2, §3.3.** Smallest and most independent first, and errors last because they ride on unions. §2 goes into phase 8, ahead of the reflection levels. §1 and §3 are new phases after phase 8.

---

## 1. `@[noalloc]`

### 1.1 Goal
`design.md` §1 requires "no ARC traffic or allocation inside an interrupt handler unless explicitly allowed" and §6.4 plans `@[callconv: interrupt]`, but nothing enforces it. scratch-lang had a `nogc` annotation for this. `@[noalloc]` is the checked version: a promise the compiler verifies, with a call-path explanation when it fails.

Non-goals: a general effect system, lock-freedom or real-time guarantees (`@[nolock]` could reuse the same machinery later), or proving stack depth.

### 1.2 The guarantee
Executing a `@[noalloc]` message, including everything it calls, **never calls the allocator to allocate or free, and performs no retain or release on a heap object.**

The second half matters: a `release` that reaches zero frees, so any owned heap reference that is dropped is a possible `free`. The rule is therefore about ownership, not only about `new`.

### 1.3 Rules inside a `@[noalloc]` body

**R1 — no creation.** No heap object, string or dynamic container is created.

**R2 — borrowed references only.** Values of class, string, dynamic-container and protocol type may be read and passed on, but never *owned*:
- allowed sources: `self`, parameters, field reads, `ifNotNil:` bindings, immortal constants (string literals, constant tables);
- forbidden: storing into a managed-typed field or into a managed-typed local after its declaration (the old value is released); sends whose result is a managed type (results are +1, owned, and released at end of statement), except synthesized field accessors, which are treated as borrowed field reads;
- a managed-typed local initialised from a borrowed expression and never reassigned is an *alias*, not an owner. The generator emits no retain or release for it.

Why borrowing is sound here: nothing in the transitive noalloc call tree can overwrite a managed field (R2 forbids it), so a +0 read cannot be invalidated mid-call. This is the "+0 field reads proven safe" optimisation `design.md` §6.1 already lists as future work, enabled only inside noalloc code.

**R3 — calls must be known noalloc** (table below).

Per operation (names are `SendKind`s from `semanticModel.h`):

| Operation | In `@[noalloc]` |
|---|---|
| arithmetic, comparison, casts, enum / subrange / bit-set ops, fixed arrays (`containerAt`, `containerAtPut` on `array of: T by: E`), records, `case`, loops, inlined blocks (`ifTrue:` family, `to:do:`, `timesRepeat:`, `ifNil:` family, `inlineDo`) | allowed |
| `constantValue`, `metaInfo`, immortal string literals, enum `name` / `printString` | allowed |
| `stringCompare`, `stringView`, `stringViewCount`, `stringByteAt`, `stringIsEmpty`, `stringPredicate`, `charValue` | allowed (borrowed receiver) |
| `newObject` / `constructObject` / `x: C;` **placed on the stack** (the analysis already records these in `model.stackObjects`) | allowed |
| the same, placed on the heap; `copy` | error `sema.noalloc.heapObject`; the note says why it escaped, or suggests `@[stack]` |
| `stringConcat`, `stringSplit`, `stringJoin`, `stringFromUtf8*`, `stringToUtf8`, `stringFromChar`, `printString` on non-enums, `dictionaryKeys`, `dictionaryValues`, non-constant `arrayLiteral` / `setLiteral` | error `sema.noalloc.allocates` |
| `containerAdd`, `containerRemove`, `arrayRemoveLast`, and `containerAtPut` on dynamic arrays, dictionaries, hashtables and sets (may grow, copy on write, or free an element) | error `sema.noalloc.allocates` |
| `directMethodCall`, `superCall`, `classStaticCall`, `vtableCall` | allowed iff **every** implementation the call can reach is noalloc (declared or inferred, see 1.5). The set is already computed for dispatch |
| `dynamicSend` (protocol) | allowed iff the **protocol message** is declared `@[noalloc]` (a contract, see 1.4) |
| `value:` on a block parameter | error `sema.noalloc.unknownTarget` in v1 (the block's effect is unknown). Locally defined blocks are fine: their body is known |
| a block literal with a stack environment (`model.stackBlocks`) | allowed: the environment is a stack record |
| a block literal that becomes a heap closure, or a variable it boxes (`model.boxedLocals`) | error `sema.noalloc.allocates`: the closure and each box are heap objects. The note gives the escape reason from `model.blockEscapes` |
| reading a `@[weak]` or `@[unowned]` field or variable | error under R2: `bobWeakLoad`/`bobUnownedLoad` return a retained (+1) reference. **[Open]** a borrowed weak read that is valid only while nothing can destroy the object |
| `@[extern: "C"]` and `@[unsafe]` messages | must carry an explicit `@[noalloc]`; it is then trusted, not checked. This is the one escape hatch |

### 1.4 Contracts on protocol messages
```
@[protocol] class IrqHandler {
    @[noalloc] msg handle: vector:uint8;
}
```
A message declared `@[noalloc]` in a protocol (or in a superclass) requires every implementation and override to be noalloc, in the same way a signature must match (`sema.overrideMismatch`). Checking is local to the implementer's module, so no whole-program step is needed. This is what makes interrupt vector tables type-safe, and it composes with itables (§2): the witness slot's call site can be proven allocation-free without knowing the target.

### 1.5 Attachment, inference and the interface
- Attachable to a message, a class (all its messages), or a module (default). Innermost wins, as with `@[overflow:]`. `@[allocates]` opts a message out of a class- or module-level default.
- `@[callconv: interrupt]` (phase 9) implies `@[noalloc]`; it is an error to opt out.
- The analyzer computes `mayAlloc` for **every** message with a fixed point over `MessageInfo::calls`, exactly like the escape summaries (`analyzerEscape.cpp`). Declared `@[noalloc]` is then a *requirement*: if the computed effect says "may allocate", report the path.
- **Only declared `@[noalloc]` goes into `.bobi`**, not the inferred effect. Writing the inferred effect would change the fingerprint every time an implementation changed. A declared attribute is sound because it was verified when its own module compiled. Adding the attribute changes the interface (it's in the body text); removing it is a breaking change.

### 1.6 Diagnostics
```
error[sema.noalloc.allocates]: Uart>>handle: is @[noalloc] but may allocate
  uart.bob:31   rx add: b            array 'add:' may grow its buffer
  note: reached via Uart>>handle: -> Uart>>store:
  note: use a fixed array (array of: uint8 by: [0 to: 255]) or make 'rx' a RingBuffer
```
`bobc --explain-effects` (like `--explain-dispatch`) lists every message's computed effect.

### 1.7 Example
```
enum Reg : uint8 { data; status; control; }

class Uart {
    private {
        rx:   array of: uint8 by: [0 to: 255];
        head: uint8;
    }

    @[noalloc] msg onInterrupt {
        b := self readReg: Reg data;       # direct call, must be noalloc
        rx at: head put: b;                # fixed array: no allocation
        head := head wrapAdd: 1u8;
    }
}
```

### 1.8 Implementation steps
1. `analyzerEffects.cpp`: the operation table above keyed by `SendKind`, plus the message-level fixed point. Reuse `calls`, the closed-hierarchy implementation sets, `stackObjects`, `stackBlocks` and `boxedLocals`.
2. R2 checks (managed stores, owned results) and the alias rule.
3. Codegen "borrow mode": skip retain/release for aliases and +0 field reads inside noalloc messages. `stackOnlyLocals` already shows the pattern.
4. Protocol contracts and `override` checks; `.bobi` read and write of the attribute; `--explain-effects`.
5. Optional debug aid: a `--check-noalloc` build wraps every `@[noalloc]` entry in a per-CPU counter and traps in the allocator if it is non-zero. This validates trusted `@[unsafe]` code.

Tests: `tests/programs/run/noalloc*.bob` (positive, including a stack object and a fixed-array ring), `semanticTests.cpp` negative cases for each diagnostic code, a two-module test for the `.bobi` round trip.

### 1.9 Open
- **[Open]** Borrowed *returns*: `@[borrowed] msg current [Part]` (result aliases `self` or a parameter) would let more code qualify. v1 allows only synthesized accessors.
- **[Open]** Block-typed parameters: block types are now written `{ int32 [bool] }`, so the effect needs a spelling there, for example an attribute on the type (`@[noalloc] { int32 [bool] }`) or a word inside the braces. The same question applies to `throws` on block types (§3.3).
- **[Open]** Whether non-atomic refcounts for single-core early boot belong here or as a separate per-module attribute.

---

## 2. Itable dispatch for protocols

**Status 2026-11-27: Stages 1 and 2 built** (all of §2.6). **Stage 2:**
- instance messages are `(self, args…)`;
- each method-table entry points at a thunk `<symbol>.msgSend(self, selector, args…)` that drops the selector and tail-calls the message;
- `tests/reflectionTests.cpp` JITs a class and calls it through `bob_msgSend`, for its own message and for an adopted protocol default.

Thunks are emitted only for classes at reflection level `invoke` (design §9). Below that, the method table lists selectors without implementations (`identity`, `introspect`) or is empty (`none`).

**Stage 1:** Every protocol send uses an itable, and the record-return restriction is lifted. `tests/programs/run/itables.bob` covers:
- a protocol message returning a record;
- a refinement upcast;
- defaults calling defaults;
- conformance through a superclass, with an override;
- an array of a protocol type.

The modules test covers dispatch across modules.

What changed from the draft:
- `BobProtocol` is `{name, messageCount}`: the conformance list holds the whole transitive set, so the lookup never needs `parents`.
- The conformance list is unsorted, and `conformanceCount` reuses the old `reserved` field. `cache` stays at offset 48, so `msgSend.asm` is unchanged. `conformances` is at 56 and the vtable moves to 64.
- §2.2 said LLVM could hoist the lookup out of loops; as first built, it didn't. The call in the loop body could change memory, so LLVM reloaded `isa` and called the lookup again on every iteration. The fix:
  - the `isa` and witness-table loads carry `!invariant.load` (a class never changes and tables are constant);
  - each send inlines a check of the class's **first** conformance entry. A protocol-send receiver always has one, so the read is always valid;
  - any other entry goes to the runtime `bobWitness`, declared `memory(none) willreturn nounwind`, which scans the list and traps on "not found".

**Microbenchmark** (300M sends, `-O2`, best of 5, against the committed tree's `bob_msgSend`):

| | itable | `bob_msgSend` | vtable (tier 1v) |
|---|---|---|---|
| one receiver in a loop | 288–290 ms | 352 ms | 287–291 ms |
| two classes alternating, protocol first or second in each list | 1183–1213 ms | 1206–1210 ms | — |

The gate in §2.6 passes:
- with one receiver, an itable send costs the same as a vtable send, and about 18% less than `bob_msgSend`;
- with polymorphic receivers it's a tie (the spread is run-to-run noise, before and after Stage 2). The scheme without invariant loads and without the inline first-entry check measured 711 ms on the first row.

### 2.1 What exists today
A send through a protocol-typed value is `SendKind::dynamicSend`, lowered to `bob_msgSend(self, selector, args…)` (`codeGenerator.cpp` around the `msgSendFunction_` call). The MASM routine probes a per-class `BobMethodCache`, and on a miss calls `bobMsgSendLookup`, which walks `methods` up the superclass chain and fills the cache. Every instance message takes `(self, selector, params…)`.

Costs, in order of importance for a kernel language:
1. **Mutable dispatch state.** The cache is written at run time, lazily, and the miss path walks tables. That is a problem in interrupt handlers and on multiple cores.
2. **It is on the hot path of the cross-module extension point.** Protocols are how modules extend each other (`design.md` §5.4).
3. **MASM, Win64-only,** sits in that path.
4. **A restriction nobody wants:** `analyzer.cpp` (`analyzeInstanceSend`, the `info->isProtocol` branch) reports "a protocol message that returns a record" as unsupported for every protocol send. My reading, not verified: on Win64 a hidden struct-return pointer takes RCX, displacing `self` from where the tail-jumping `bob_msgSend` expects it.
5. The selector argument is passed to every method but dispatch is static elsewhere. **Checked 2026-11-27:** no generated body reads it, so dropping it (§2.4) is safe.

Static typing makes dynamic selector lookup unnecessary here: the conformance set of every class is known when its module compiles.

### 2.2 Design
**Protocol descriptor**, one per protocol, `linkonce_odr`, identity by address (the same trick selectors already use):
```c
struct BobProtocol {
    const char*              name;
    uint32_t                 messageCount;   // including inherited protocols' messages, which come first
    uint32_t                 parentCount;
    const BobProtocol* const* parents;
};
```
**Witness table**, one per (class, protocol) pair, read-only data: an array of `messageCount` function pointers in the protocol's canonical slot order.

**Conformance list** in the class:
```c
struct BobConformance { const BobProtocol* protocol; void* const* witness; };
// BobClass gains:  const BobConformance* conformances;  uint32_t conformanceCount;  (sorted by protocol address)
```
`classVtableOffset` in `abi.h` moves from 56 to match (or reuse the `cache` slot once nothing else needs it). The value stays a **thin pointer**: protocol-typed references remain one pointer, so fields, optionals (nullable pointer), `BobElementType` and ARC are untouched.

**Call site** (replaces the `dynamicSend` branch only):
```
%isa = load ptr, ptr %self
%w   = call ptr @bob.witness(ptr %isa, ptr @protocol.Shape)   ; readonly, nounwind, speculatable
%fn  = load ptr, ptr (gep %w, slot*8)
%r   = call %fn(ptr %self, args...)                            ; typed indirect call
```
`bob.witness` scans `conformances` linearly for small counts and binary-searches larger ones; "not found" traps and cannot happen in well-typed code. Because it is a pure read of immutable data, LLVM can hoist it out of loops.

### 2.3 Rules
- **Slot order:** parent protocols' messages first, then the protocol's own in declaration order. Adding or reordering required messages changes the protocol's interface and fingerprint, so importers recompile. That is the existing Pascal-style rule.
- **Defaults:** a class that doesn't override gets the protocol default (`bob.<Protocol>.<selector>`, which already exists) in its slot. Defaults that send to `self` use the itable again.
- **Inheritance:** a subclass gets its own witness table per protocol in its transitive conformance set, resolved to its own overrides. (Tables are small and in `.rdata`.)
- **Refinement:** `Scalable inherits Shape` has Shape's slots as a prefix, so a Scalable-typed value sends Shape messages through Scalable's table. A class conforming to Scalable also lists Shape, aliasing the same array.
- **Cross-module:** a class conforms through its own module's `implements`, so the list is complete when that module compiles. Retroactive conformance from another module is already unsupported (`extends` on imported classes). **[Open]** the future hook is a link-section registry (`.bobconf$…`) consulted only when a class's own list misses.
- **Reflection:** `bob_msgSend` and the method table stay for `perform:` and dynamic loading (design §9). With `@[reflect:]` at "none", the selector table need not be emitted at all, which matches the "metadata that isn't emitted doesn't exist" rule. Phase 8's reflection levels are designed on top of this, so they come after §2.
- **`--explain-dispatch`:** report `tier 2 (itable Shape slot 0)`; `perform:` is `tier 3`.

### 2.4 Optional follow-up: drop the selector parameter
Stage 2 of the migration removes `selector` from instance-message signatures (smaller code, one register back) and gives reflection small generated thunks `(self, sel, args) -> direct(self, args)`, emitted only for classes whose reflection level needs them.

### 2.5 What this unlocks
- Protocol messages that return records (remove the `unsupported` at `analyzer.cpp` ~line 807, after verifying the sret theory with a test).
- No mutable state in dispatch, so it is safe from interrupt handlers and SMP.
- Tables are read-only, so a kernel can place them in ROM or protect them.
- `@[noalloc]` contracts (§1.4) can be proven across a protocol call.
- MASM leaves the hot path, so the dispatch core becomes portable LLVM IR.

### 2.6 Implementation steps and tests
1. Runtime/ABI: `BobProtocol`, `BobConformance`, `bob.witness`, offsets in `abi.h`.
2. Codegen: emit descriptors and per-class witness tables where the class info is emitted; replace the `dynamicSend` branch. Keep ABI `(self, selector, …)` in this step so it is a drop-in change.
3. Lift the record-return restriction; add tests.
4. (Stage 2) drop `selector`, add reflection thunks.

Tests: existing `polymorphism.bob` unchanged and passing; new run programs for a protocol returning a record, refinement upcast, defaults calling defaults, an array of a protocol type, and a two-module implementation (`tests/programs/modules`). Gate: every run program leak-free at `-O0` and `-O2`. Add a small microbenchmark (protocol send in a loop, old versus new) before committing to the claim that it is no slower.

### 2.7 Alternatives considered
- **Fat pointers** (`{object, witness}`, as in Swift, Go and Rust): fastest call, no lookup. But every protocol-typed field, element, optional and ARC operation changes shape. Defer; the thin-pointer scheme can be upgraded later without changing the language.
- **Global dense protocol ids assigned at link time:** gives O(1) tables but needs whole-program knowledge, which separate compilation lacks.
- **Keep the cache and make it thread-safe:** still needs allocation and a slow path in interrupt context.

---

## 3. Tagged unions and error handling

### 3.1 Why these belong together
There are no unions: inferred unions were removed on 2026-11-27, because the type appeared without being asked for and couldn't be written down (`design.md` §4.5). Named unions answer both objections: they're declared, named and written in signatures. Enums carry no data and tuples aren't implemented. So there is no way to write a `Result`, a token, an AST node or a device message. Errors are the most important consumer: a failable message returns "a value or an error", which is a union.

### 3.2 Named tagged unions

**Declaration** (same shape as `record`; payload-less variants look like enum members):
```
union Token {
    number: int64;
    word:   string;
    open;
    close;
}

union Token : uint8 { ... }      # optional tag storage type, as for enums
```
- Each variant has a name and at most one payload type. Several values go in a record.
- Variant names are scoped to the union, like enum members (`Token open`).

**Construction** is a generated static message, consistent with `Point x: 1 y: 2` and `Color red`:
```
t := Token number: 42;           # an untyped literal adopts the payload type
u := Token open;
```

**Matching** reuses `case` and its exhaustiveness rule (`semantics.md`, enums and inferred unions). Labels are variants; a single-label entry may bind the payload as a block parameter:
```
case t of {
    Token number: { [n] total := total + n; }
    Token word:   { [w] names add: w; }
    Token open, Token close: { depth := depth + 1; }
}
```
No `else` is needed when every variant is covered; the error lists up to five missing variants as it does for enums. The parser already splits `green:` into label and colon for enum labels, and still parses an entry body as a block literal, so I expect no parser change, only semantic resolution of the label as a variant (to verify). Binding a value in a `case` entry is currently `sema.invalidCaseBinding` everywhere; it comes back for union subjects only.

**Queries.** `t number` gives `int64?` (the payload or `nil`), and `t is: Token open` gives `bool`. **[Open]** the `is:` spelling.

**Layout.** `{ tag, payload }`; tag is the smallest unsigned type that fits (or the declared one) and comes first, payload at the maximum member alignment and size. `meta size`, `meta alignment` and `layout.cpp` get a new case, so layout asserts work.

**ARC.** The compiler generates a copy function (retain by tag) and a destroy function (release by tag) per union, and containers reuse the existing `BobElementType` retain/release callbacks (`elementCallback` in `codegenValues.cpp` handles aggregates already). Payloads may be references, strings, containers, optionals and blocks. Copying a union retains its payload; payload objects keep reference semantics.

**Recursion.** A union that directly contains itself is `sema.recursiveUnion`, with a hint to hold the recursive part in a class field (`class AddNode { lhs: Expr; rhs: Expr; }` and `union Expr { number: int64; add: AddNode; }`). There is no hidden boxing and no hidden allocation.

**Relationship to existing features.** `T?` stays built in. There are no inferred unions to share code with: the removed implementation (tag + inline payload, a switch on the tag) is a reasonable model for the layout, but named unions start fresh. A mixed literal stays an error (`sema.mixedElementTypes`); it does **not** infer a named union.

### 3.3 Error handling

**Model.** Errors are **values returned through the normal return path**: no exceptions, no unwinding, no tables, no runtime support, and no change to ARC cleanup. This matches `design.md` §5.6 (point 4). Runtime *traps* (overflow, bounds, divide by zero, subrange) stay fatal.

**Declaring failability.** After the return type:
```
msg open: path:string [File] throws IoError { ... }
msg poll [uint8] throws DeviceError { ... }
msg reset throws DeviceError { ... }          # no result value
```
`throws` is a contextual word, like `msg`.

**Error types** are value types: an enum, a record, a named union, a distinct numeric type or a subrange. **v1 restricts them to plain data** (no managed fields), so the error path needs no allocation and no refcounting and works inside `@[noalloc]`. **[Open]** allow managed payloads once §3.2's ARC glue exists. Typed errors, not an `Error` protocol: a protocol would need boxing (allocation) and dynamic type tests.

**Raising.** `fail expr;` is a statement like `return`; `expr` converts to the declared error type.
```
if status = Status timeout {
    fail DeviceError timedOut;
}
```

**Using a failable send.** Every failable send must be covered, or it is `sema.uncaughtFailure`. There are two ways.

1. **`try expr` propagates.** A prefix with the lowest precedence, like `return`'s operand. It covers every failable send in its operand (not inside non-inlined blocks). On failure it returns the error from the enclosing message, which must itself be failable with a compatible error type.
   ```
   f    := try File open: path;
   data := try f readAll;
   n    := try self parse: a + self parse: b;       # one try covers both sends
   ```
2. **Inlined handlers**, the same pattern as the existing `ifNil:` / `ifNotNil:` family, so no new statement syntax:
   ```
   n := (self parse: text) ifError: { [e] return -1; };
   (self poll) ifOk:    { [b] self store: b; }
               ifError: { [e] self noteFault: e; };
   ```
   Blocks are inlined (no closure), `return` and `fail` are allowed inside them (they run in the message's own frame), and a value-producing handler must end in an expression of the result type or diverge.

**Error conversion for `try`.** The inner error must be the same type as the outer one, or the outer type must be a named union with **exactly one** variant whose payload is the inner type (the compiler wraps it). Otherwise `sema.errorMismatch`, which names both types and suggests an `ifError:` handler.
```
union ParseError { badDigit: char; overflow; io: IoError; }

msg readHeader: p:Port [Header] throws ParseError {
    magic := try p read;                  # IoError -> ParseError io:, unambiguous
    if magic <> 0x7F { fail ParseError badDigit: 'x'; }
    ...
}
```

**Optional reification.** ~~`r := (self parse: text) outcome;` giving the closed union `T | E`~~: dropped, because it would bring back an unnamed inferred union (and a `|` spelling, which bob avoids). **[Open]** if reification is wanted, it returns a declared union the program names.

**Rules.**
- `throws` is part of a message's signature: required messages in protocols and overrides must match exactly (`sema.overrideMismatch`).
- `try`, `fail` or a handler's `return` inside a non-inlined block is `sema.tryInBlock`, for the same reason as `sema.returnInBlock` (it would have to unwind through the block's caller). Use a `for` loop. **[Open]** `throws` on a block type, which is now written `{ T [R] }` (see §1.9 for the same spelling question).
- `init` can't throw. Construction that can fail stays an ordinary `@[static]` message returning `T throws E` (as `design.md` §4.3a already prescribes).
- `main` can't throw in v1. **[Open]** whether an error enum maps to an exit code.

### 3.4 Lowering
A failable message returns a tagged `{tag, value or error}` layout, the same one a named union uses (tag 0 = value). Small results come back in registers on Win64 (up to 16 bytes); larger ones use a struct-return pointer, which is why §2's typed indirect calls matter for protocol messages. `try` is one compare of the tag and a branch to the **same cleanup code `return` already uses** (scope-exit releases), then `ret {1, error}`. The success path pays one predictable branch. The leak report in the test harness checks cleanup on both paths.

### 3.5 How the pieces fit together
```
@[protocol] class Block {
    @[noalloc] msg readWord: sector:uint32 [uint32] throws DiskError;   # contract, typed slot, no allocation
}
```
The protocol message is a noalloc contract (§1.4), dispatches through an itable slot (§2), and returns a value-type error (this section) with no allocation on either path. Every implementer must also be noalloc and declare the same `throws`.

### 3.6 Rejected alternatives
- **Exceptions with unwinding:** tables, runtime support and hostile to kernels and C boundaries (the stated reason in §5.6).
- **Untyped errors / an `Error` protocol:** boxing, allocation, dynamic checks; typed errors keep `case` exhaustive.
- **`Result of: T error: E` as a library type:** needs type-returning compile-time messages (planned, not built) and makes every signature verbose; it gives no `try`. It can be added later as sugar over `throws`.
- **Optionals only:** loses the reason for failure.
- **A catch statement (`do { } catch`):** not needed for v1. The inlined handlers cover the single-call case, and a common handler for several calls is a small failable helper message. **[Open]** revisit after real use.

### 3.7 Implementation steps
1. **U1:** `union` declaration, constructors, `case` matching and exhaustiveness, layout and `meta`, plain-data payloads only, `.bobi`.
2. **U2:** ARC glue for managed payloads.
3. **E1:** parser (`throws`, `try`, `fail`, the `try` precedence), message signature and override checks, the "covered" analysis, conversion and wrapping rules.
4. **E2:** codegen (failable return layout, `try` branch to shared cleanup), handler intrinsics beside the `ifNil:` family, `.bobi`.
5. **E3:** `sema.*` diagnostics and tests: success and failure paths for each form, leak-free at `-O0` and `-O2`, an error crossing a module boundary, an error through a protocol call.

### 3.8 Open (summary)
`is:` spelling; reification only as a declared union (`outcome` as specified is dropped); managed-payload errors; `throws` on block types; exit codes for `main`; tuples (`[int32, string]` returns, still `unsupported` in `analyzer.cpp`) are independent of this work. They could lower to anonymous records and would compose with `throws` unchanged.

---

## 4. Not verified
- No backend build or test run; claims about current behaviour come from reading the code and docs.
- The Win64 sret explanation in §2.1 is my inference; step 3 of §2.6 should start with a test that reproduces the restriction.
- I did not read the parser's case-label code in detail for §3.2, only the grammar document.
- The performance comparison in §2.6 is a gate, not a measurement.
