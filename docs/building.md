# Building bob

## Requirements
| Tool | Used for | Where it's found |
|---|---|---|
| Visual Studio 2026 (MSVC `cl`, MASM `ml64`) | building the compiler, runtime and tests | VS developer environment (`vcvars64.bat`) |
| CMake ≥ 3.30, Ninja | build system | bundled with Visual Studio |
| LLVM 22.1 or later (presets use 23.1.2), Debug and Release builds | code generation library (`bobCompiler`), `llvm-dlltool` | `BOB_LLVM_ROOT` (set by the presets) |
| `lld-link` | linking bob programs | `C:\Program Files\LLVM\bin` (found at configure time, else PATH, else `--linker=`) |
| GoogleTest 1.15.2 | `bobTests` | downloaded by CMake `FetchContent` at configure time |

Debug builds of bob must link against the **Debug** LLVM build and Release builds against the **Release** build. MSVC's debug and release C runtimes and STL layouts can't be mixed, which is why the presets point at different LLVM roots.

The prebuilt `clang+llvm-<version>-x86_64-pc-windows-msvc.tar.xz` from the LLVM GitHub releases works as the Release root (it includes `lib/cmake/llvm`; the `LLVM-<version>-win64.exe` installer does not). LLVM ships no Debug build, so a Debug root has to be built from source. Tested with 22.1.7 (Release) and 23.1.2 (Debug and Release).

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
| `bobrtHosted.lib` | the same runtime core built for the compiler's own process; linked into `bobc` so compile-time code (ORC JIT) runs against it. Not shipped with programs |
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
bobc --reflect-max=none prog.bob   # emit no reflection metadata (none, identity, introspect, invoke)
bobc --syntax-only prog.bob        # report syntax errors only (exit 1 if any)
bobc --dump-tokens prog.bob        # token stream
bobc --dump-ast prog.bob           # syntax tree, text
bobc --dump-ast=json prog.bob      # syntax tree, JSON (see docs/tooling.md)
```
A `module` or `lib` unit builds into a static library plus its interface. Build the modules first, then the program that imports them:
```
bobc geometry.bob                  # → geometry.lib + geometry.bobi
bobc -c geometry.bob               # → geometry.obj + geometry.bobi
bobc shapes.bob                    # a lib importing geometry: finds geometry.bobi next to shapes.bob
bobc app.bob --module-path=libs    # also searches libs\ for interfaces; links each module's .lib
```
After changing only a module's implementation, rebuild just that module. If its interface changed, `bobc` says which dependents to recompile ("'shapes' was compiled against a different interface of 'geometry'"). `tests/programs/modules` has a working example.

The grammar is in `docs/grammar.md`, the type rules in `docs/semantics.md`. Runtime traps (overflow, division by zero…) print to stderr and exit with code 101. If objects are still allocated when `main` returns, the runtime prints `bob: leak: …` to stderr.

## Test programs
`tests/programs/run/*.bob` are compiled and run by `bobTests` at `-O0` and `-O2`. Each declares its outcome in header comments: `# expect-exit: <code>` and optionally `# expect-stderr: <text>`. Every program must also exit without a leak report, unless its expected stderr is the leak report itself. To add a test, drop in a new file. No other registration is needed.

`tests/programs/unicode/GraphemeBreakTest.txt` is Unicode's conformance data for grapheme clusters. `Unicode.GraphemeBreakConformance` runs every case against the runtime's segmenter (`src/runtime/core/unicode.cpp`, compiled into `bobTests` too).

## Unicode tables
`src/runtime/core/unicodeTables.inc` (grapheme break properties, Extended_Pictographic, Indic_Conjunct_Break) is generated from the UCD files in `tools/unicodegen/ucd` (Unicode 16.0.0) and checked in. To regenerate it after updating those files, run this from a VS developer prompt in the repository root:
```
cl /std:c++20 /EHsc /O2 tools\unicodegen\unicodegen.cpp /Fe:unicodegen.exe
unicodegen.exe tools\unicodegen\ucd src\runtime\core\unicodeTables.inc
```
Update `tests/programs/unicode/GraphemeBreakTest.txt` to the same Unicode version.

## Using bobc on LLVM IR (phase 0)
`bobc` accepts hand-written LLVM IR that defines `i32 @bobMain()`:
```
bobc hello.ll                  # → hello.exe
bobc -c hello.ll -o hello.obj  # object only
bobc -v hello.ll               # also print the lld-link command
```
Programs are linked with `/entry:bobStart /nodefaultlib` against `bobrt.lib` and `kernel32.lib` only. The resulting exe imports nothing but kernel32.

`.obj` and `.lib` files after the input are linked in too: assembled code (design §6.7) or more import libraries. A module packs its `.obj` inputs into its own static library:
```
ml64 /c /Fo switch.obj switch.asm
bobc kernel.bob switch.obj      # a program: linked into kernel.exe
bobc tasks.bob switch.obj       # a module: switch.obj goes into tasks.lib
``` Runtime functions available to IR in phase 0:
```
declare void @bobRuntimeWrite(i32 %stream, ptr %bytes, i64 %count)   ; stream 1 = stdout, 2 = stderr; UTF-8 bytes
```
