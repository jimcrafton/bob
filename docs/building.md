# Building bob

## Requirements
| Tool | Used for | Where it's found |
|---|---|---|
| Visual Studio 2026 (MSVC `cl`, MASM `ml64`) | building the compiler, runtime and tests | VS developer environment (`vcvars64.bat`) |
| CMake ≥ 3.30, Ninja | build system | bundled with Visual Studio |
| LLVM 23.1.2, Debug and Release builds | code generation library (`bobCompiler`), `llvm-dlltool` | `BOB_LLVM_ROOT` (set by the presets) |
| `lld-link` | linking bob programs | `C:\Program Files\LLVM\bin` (found at configure time, else PATH, else `--linker=`) |
| GoogleTest 1.15.2 | `bobTests` | downloaded by CMake `FetchContent` at configure time |

Debug builds of bob must link against the **Debug** LLVM build and Release builds against the **Release** build. MSVC's debug and release C runtimes and STL layouts can't be mixed, which is why the presets point at different LLVM roots.

## Presets
| Preset | Build type | LLVM root |
|---|---|---|
| `x64-debug` | Debug | `D:/code/LLVM-23.1.2_debug` |
| `x64-release` | RelWithDebInfo | `D:/code/LLVM-23.1.2` |

To use a different LLVM location, override `BOB_LLVM_ROOT` in a `CMakeUserPresets.json` (git-ignored), or pass `-DBOB_LLVM_ROOT=...`.

## Command line
From a VS developer prompt (`vcvars64.bat`):
```
cmake --preset x64-debug
cmake --build --preset x64-debug
out\build\x64-debug\bin\bobTests.exe
```
Visual Studio opens the folder directly and picks up the presets.

## Outputs (`out/build/<preset>/bin`)
| File | What |
|---|---|
| `bobc.exe` | compiler driver |
| `bobrt.lib` | runtime, freestanding (no CRT) |
| `kernel32.lib` | import library generated from `src/runtime/platform/win64/kernel32.def` by `llvm-dlltool` |
| `bobTests.exe` | GoogleTest unit and end-to-end tests |

`bobc` looks for `bobrt.lib` and `kernel32.lib` next to itself by default (`--runtime-dir=` overrides this). If the generated `kernel32.lib` is missing, it falls back to the newest Windows SDK `kernel32.lib`, or to `--sdk-lib-path=`.

## Using bobc on bob source
A `program` unit compiles to an executable (phase 2: primitives, records, static messages, control statements, see `docs/semantics.md`):
```
bobc prog.bob                      # → prog.exe
bobc -O2 prog.bob -o fast.exe      # optimize (-O0 default, -O1..-O3)
bobc -c prog.bob                   # object file only
bobc --dump-llvm prog.bob          # print the generated LLVM IR instead of compiling
bobc --explain-dispatch prog.bob   # also report how each send and block is compiled
bobc --syntax-only prog.bob        # report syntax errors only (exit 1 if any)
bobc --dump-tokens prog.bob        # token stream
bobc --dump-ast prog.bob           # syntax tree, text
bobc --dump-ast=json prog.bob      # syntax tree, JSON (see docs/tooling.md)
```
The grammar is in `docs/grammar.md`, the type rules in `docs/semantics.md`. Runtime traps (overflow, division by zero…) print to stderr and exit with code 101.

## Test programs
`tests/programs/run/*.bob` are compiled and run by `bobTests` at `-O0` and `-O2`. Each declares its outcome in header comments: `# expect-exit: <code>` and optionally `# expect-stderr: <text>`. To add a test, drop in a new file. No other registration is needed.

## Using bobc on LLVM IR (phase 0)
`bobc` accepts hand-written LLVM IR that defines `i32 @bobMain()`:
```
bobc hello.ll                  # → hello.exe
bobc -c hello.ll -o hello.obj  # object only
bobc -v hello.ll               # also print the lld-link command
```
Programs are linked with `/entry:bobStart /nodefaultlib` against `bobrt.lib` and `kernel32.lib` only. The resulting exe imports nothing but kernel32. Runtime functions available to IR in phase 0:
```
declare void @bobRuntimeWrite(i32 %stream, ptr %bytes, i64 %count)   ; stream 1 = stdout, 2 = stderr; UTF-8 bytes
```
