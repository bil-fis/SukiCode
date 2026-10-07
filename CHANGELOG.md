# Changelog

All notable changes to SukiCode are documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/),
and this project adheres to [Semantic Versioning](https://semver.org/).

## [0.0.0-alpha.0.20261007] - 2026-10-07

First **public pre-release (alpha)** of the SukiCode toolchain. The compiler
(`sukic`), runtime, and core standard library are functional and exercised by
an automated test suite (**92 passing tests**, 110 `.suki` files parsing
cleanly).

> The language content is unchanged from `0.0.0-alpha.0`; only the **release tag
> name** carries the date suffix (`v0.0.0-alpha.0.20261007`) so successive
> alpha builds stay distinguishable. Tags follow `v<version>.<YYYYMMDD>`.

This build delivers the language **core**, the **type system**, **ARC memory
management**, and the **concurrency subsystem** end-to-end. Many specification
features are intentionally absent in this alpha — see
[README.md → Known Limitations](../README.md#known-limitations).

### Added

**Language core**
- AOT compilation via the LLVM backend (`--emit-llvm`, `--emit-ast`, object, executable).
- Control flow: `if`/`guard`/`switch` (pattern matching), `for-in` (tuple destructuring), `while`, `repeat-while`, `do-catch`.
- `select` statement for channel multiplexing.
- Closures: Swift-style `{ in }`, arrow `(...) => ...`, capture lists `[weak self]` / `[unowned self]`, trailing closures.

**Type system**
- Value types `struct` / `enum`; reference types `class` / `actor`.
- Optionals `T?` with `?.`, `!`, `??`; generics with `where`, `associatedtype`, `some`.
- Protocols, extensions, `typealias`.

**Memory management**
- Deterministic ARC for `class` / `actor`; `weak` (auto-nil) and `unowned`.
- `Owned<T>` move semantics; `unsafe` blocks and `UnsafePointer` family.
- Pool / `MemoryPool<T>` lifecycle helpers (`memory` module).

**Concurrency (most complete subsystem)**
- `async` / `await` via LLVM coroutine intrinsics.
- `Task`, `TaskGroup` (`withTaskGroup`), `Channel<T>` (bounded/unbounded, `for await`).
- `actor` isolation (serialized message passing) and `select`.
- `Atomic<T>`, `Mutex`, `Semaphore`, `Condition`, `Future` / `Promise`.

**Error handling**
- `throws` / `do-catch` / `try` / `try?` / `try!`; `Error` protocol; `Result<T, E>`.

**Macros (partial)**
- `@freestanding(expression)` / `@freestanding(declaration)`; `@attached(member)`.
- `#makeExpr` / `#makeDecl` with `\(arg)` unquoting; hygienic `#unique`.
- Sandboxed expansion (no file/network/process/syscall).

**C / foreign interop**
- `extern "C" func` / `extern "C" { }`, variadic `...`, `@convention("C"|"stdcall")`, `@_cdecl`.

**Standard library**
- `core` (collections, strings, numerics, randomness), `system` (File/Path/Date/Clock/OS/process), `concurrency`, `data` (Data/Base64/Hex), `memory`, `test` (TestFramework).

**Tooling**
- `sukic` multi-backend compiler with `--target`, `--incremental`, `-expand-macros`.
- `suki-lsp`, `suki-doc` (functional).
- `sukipm` `init` / `build` / `test` (functional).
- `suki-fmt` (placeholder pass-through).
- CMake build and the `run_all_tests.sh` test runner. No third-party C++
  dependency is linked (no `find_package` call in the tree), so **vcpkg is not
  required** to build.

**CI / distribution**
- GitHub Actions pipeline (`.github/workflows/ci.yml`): every push to
  `main`/`master` and every pull request runs `./run_all_tests.sh`; pushing a
  `v*` tag additionally builds and publishes the release artifacts below.
- Standalone, **relocatable** toolchain archives published with each tagged
  release: `sukicode-linux-x86_64.zip` and `sukicode-windows-x86_64.zip`. Each
  ships `sukic`, the prebuilt runtime object, the standard-library sources, the
  bundled `libLLVM` (Linux only; Windows links LLVM statically) and a `suki`
  launcher script or batch file. The only host requirement is `clang` on `PATH`.
- `sukic` can now locate its runtime and standard library **relative to the
  archive** instead of relying on build-machine absolute paths:
  `SUKICODE_STDLIB_DIR`, `SUKICODE_RUNTIME_OBJECT`, `SUKICODE_RUNTIME_SOURCE`,
  `SUKICODE_RUNTIME_INCLUDE` and `SUKICODE_CLANG_DRIVER` override the values
  CMake bakes into the binary (when unset, behavior is exactly as before).
- VS Code extension skeleton in `idePlugins/vscode/`, shipped as
  `sukicode-vscode-<version>.vsix`: a TextMate grammar providing SukiCode
  syntax highlighting. The LSP client/server scaffolding is prepared but not
  yet activated.

### Changed

- **Printing convention (spec §12.1):** `print` now appends a trailing newline by
  default; `printsl` ("single-line") prints **without** a trailing newline.
  The previous `println` builtin was **removed** in favor of `print` (newline)
  and `printsl` (no newline).

### Fixed

- **Module-qualified access (internal: D1).** Calls such as `system.Clock.now()`
  or `system.Path(...)` were incorrectly resolved — the module name was treated
  as an *instance* rather than a module/type, producing wrong types (e.g. `ptr`
  instead of `Int`). They are now resolved as static/module member access and
  routed directly to the runtime entry points.
- **Coroutine-frame / loop-variable scope bug.** A `for await` (and synchronous
  `for-in`) loop variable was left bound in `locals_` after the loop ended. A
  later closure (e.g. a `Task { ... }`) implicitly captured that out-of-scope
  variable, emitting a `store` into a block the variable's `alloca` did not
  dominate and failing LLVM IR verification
  (`Instruction does not dominate all uses!`). Loop variables are now dropped
  from `locals_` (with an ARC release for reference types) at the loop's exit
  block, so the capture no longer happens. Reproducible with two `for await`
  loops in one `async func`, or a `for await` followed by `withTaskGroup`.
- **Codegen unit test for the printing convention.** After `print` / `printsl`
  semantics changed (see above), `test_codegen` still asserted that `print`
  lowered to `suki_print_str`, which made the suite red. The expectation now
  pins **both** runtime entry points **in both directions** (`print` →
  `suki_println_str` and *not* `suki_print_str`; `printsl` → `suki_print_str`
  and *not* `suki_println_str`), so neither branch can silently regress again.

### Known Limitations

See [README.md → Known Limitations](../README.md#known-limitations). Notably
**not** in this build: `defer` code generation, `#if` conditional compilation,
`@autoclosure`, most `@attached` macro roles, `Error.localizedDescription`,
`MMapRegion` / `DynamicLibrary` / `sys` module, a `ThreadPool`/`DispatchQueue`
type, the Objective-C runtime bridge, the `network`/`crypto`/`i18n`/`cli`/`SukiUI`
standard-library modules, real `suki-fmt` formatting, and SukiPM `publish`/`add`.

---

## [Unreleased]

Tracked for upcoming pre-releases (not yet committed to a version):

- Implement `defer` code generation.
- Implement `#if os/arch` conditional compilation and `-D` defines.
- Expand `@attached` macro roles (peer / access / conformance / extension).
- Wire up `suki-fmt` real formatter; SukiPM `publish` / `add`.
- Add `network` / `crypto` / `i18n` / `cli` standard-library modules.
