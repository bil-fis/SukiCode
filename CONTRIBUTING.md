# Contributing to SukiCode

Thank you for your interest in SukiCode! This document explains how to build
the project, run the tests, and propose changes.

> **Note:** SukiCode is in **alpha** (`v0.0.0-alpha.0.20261007`). APIs and internal
> interfaces are still evolving. Please check [README.md](README.md) and
> [CHANGELOG.md](CHANGELOG.md) before investing in large contributions, and
> consider opening an issue first to align on direction.

## Code of Conduct

Be respectful and constructive. We want SukiCode to be a welcoming project for
contributors of all backgrounds.

## Getting Started

### Prerequisites
- CMake ≥ 3.20
- C++20 compiler (MSVC 2022, GCC 12+, or Clang 15+)
- LLVM ≥ 15.0 with development headers (provides `llvm-config`)
- `clang` on `PATH` — the `sukic` driver shells out to it for the link stage

No third-party C++ libraries are linked: the tree contains **no `find_package`
call**, so **vcpkg is not needed** to build. (`vcpkg.json` still declares
dependencies, but none of them is consumed today; the unit tests do **not** use
gtest either — they are hand-rolled check counters.)

See [README.md → Quick Start](README.md#quick-start) for LLVM install commands
per platform.

### Building
```bash
git clone https://github.com/SukiCode/SukiCode.git
cd SukiCode
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
```

### Running the tests
```bash
./run_all_tests.sh          # builds if needed, then runs the suite
```
The suite compiles and runs SukiCode programs under `moduleTest/` (and a set of
"pending" examples) and expects **exit code 0** for positive tests, plus a
parse-verification pass over all `.suki` files under `src/`, `moduleTest/`, and
`examples/`. Current baseline: **92 passing tests, 0 failures**, with all
**110** `.suki` files parsing cleanly.

For a single file:
```bash
./build/bin/sukic run path/to/file.suki
```

## CI & Release Process

The pipeline lives in `.github/workflows/ci.yml` and runs in two modes:

| Trigger | What runs |
|---------|-----------|
| Push to `main`/`master`, or any PR | `test` — `./run_all_tests.sh` on Ubuntu (LLVM + clang installed with apt) |
| Push a tag starting with `v` | `test`, then `release` (Linux + Windows archives), `vscode` (`.vsix`), and finally `publish`, which attaches all three artifacts to a GitHub Release |

Tags follow `v<version>.<YYYYMMDD>` — for example `v0.0.0-alpha.0.20261007`. To
cut a release:

```bash
git tag v0.0.0-alpha.0.20261007
git push origin v0.0.0-alpha.0.20261007
```

Each archive ships `bin/sukic`, the prebuilt runtime object, the standard-library
sources, and (Linux) the `libLLVM` it was linked against. A launcher
(`bin/suki` / `bin\suki.bat`) sets `SUKICODE_STDLIB_DIR`,
`SUKICODE_RUNTIME_OBJECT`, `SUKICODE_RUNTIME_SOURCE`, `SUKICODE_RUNTIME_INCLUDE`
and `SUKICODE_CLANG_DRIVER`, so the archive stays usable from any directory.
Those variables override the absolute paths that CMake bakes into the binary at
build time; when they are unset, `sukic` behaves exactly as before.

Reproduce either archive locally (requires a finished `build/` configured as
Release with `SUKICODE_BUILD_TESTS=OFF`):

```bash
bash .github/scripts/package_linux.sh sukicode-linux-x86_64
pwsh .github/scripts/package_windows.ps1 sukicode-windows-x86_64
```

## Project Layout

| Path | What lives there |
|------|-----------------|
| `src/compiler/` | The `sukic` compiler: lexer, parser, AST, sema, codegen, macro engine, diagnostics |
| `src/runtime/` | C runtime: ARC, coroutines, concurrency primitives, pool |
| `src/stdlib/` | Standard library, written **in SukiCode** (`core`, `system`, `concurrency`, `data`, `memory`, `test`) |
| `tools/` | `suki-lsp`, `suki-doc`, `sukipm`, `suki-fmt` |
| `moduleTest/` | Integration tests (`*.suki`) organized by stage (lexer/parser/codegen/integration) |
| `examples/` | Runnable example programs |
| `tests/unit/` | C++ unit tests for lexer, parser, sema, codegen and runtime |
| `.github/workflows/` | GitHub Actions pipeline (`ci.yml`) |
| `.github/scripts/` | Standalone archive packagers (`package_linux.sh`, `package_windows.ps1`) |
| `idePlugins/vscode/` | VS Code extension: grammar today, LSP client later |

## Coding Conventions

### C++ (compiler & runtime)
- C++20, formatted consistently with the rest of the tree.
- Prefer `const` / `constexpr` where possible; avoid raw owning pointers.
- Add a short comment for non-obvious IR-generation or ABI decisions.
- Keep `#include`s sorted and minimal.

### SukiCode (standard library)
- Follow the style used in existing `src/stdlib/**/*.suki` files.
- Public API uses `public` where appropriate; document behavior with `///` comments.
- Prefer `@intrinsic` declarations plus a runtime implementation for primitives
  that need C-level support.

## Adding a Standard-Library Module

1. Create `src/stdlib/<module>/<module>.suki`.
2. Declare public types/functions; mark runtime-backed ones `@intrinsic` and
   implement them in `src/runtime/runtime.c` / `runtime.h`.
3. Register any compiler-builtins in `src/compiler/sema/Sema.cpp` and route them
   in `src/compiler/codegen/IRGeneratorImpl.cpp` (see how `print` / `printsl`
   are handled).
4. Add an integration test under `moduleTest/codegen/`.

## Reporting Bugs & Requesting Features

- **Bugs:** open an issue with a minimal `.suki` reproducer and the `sukic`
  command used. Include the LLVM/clang version and OS.
- **Features:** open an issue describing the use case. For large spec features,
  link to the relevant section of `SukiCode_Specification.md`.

## Pull Request Process

1. Fork and create a topic branch (e.g. `fix/defer-codegen`).
2. Keep changes focused; one logical change per PR.
3. Ensure `./run_all_tests.sh` passes (and `cmake --build` is warning-free for
   your compiler).
4. Update `CHANGELOG.md` under `## [Unreleased]` (or the relevant version) with
   an `Added` / `Changed` / `Fixed` line.
5. For user-visible behavior changes, also update `README.md` (and
   `Known Limitations` if you add or remove a gap).
6. Open the PR with a clear description and reference any related issue.

## License

By contributing, you agree that your contributions are licensed under the same
licenses as the corresponding SukiCode components:

- Compiler, runtime, and standard library contributions → Apache-2.0 **WITH**
  Runtime Library Exception.
- Tooling contributions (`suki-lsp`, `suki-doc`, `sukipm`, `suki-fmt`) → MIT.
- Documentation and examples → MIT.

See [LICENSE](LICENSE), [LICENSES/](LICENSES/), and [NOTICE](NOTICE) for the
full license texts.

---

Questions? Open a discussion/issue on the repository. Happy hacking!
