> **⚠️ Pre-release / Alpha — v0.0.0-alpha.0.20261007**
>
> SukiCode is in **public alpha**. The compiler, runtime, and core standard
> library are functional and covered by an automated test suite (**92 passing
> tests**; **110** `.suki` files parsing cleanly). However, **many features in
> the language specification are not implemented yet.** This README describes
> **what actually works today**, not the entire specification.
>
> Before relying on a feature, check [Known Limitations](#known-limitations)
> and [CHANGELOG.md](CHANGELOG.md). For the full design, see
> [SukiCode_Specification.md](SukiCode_Specification.md).

# SukiCode

A modern systems programming language that fuses Swift-, TypeScript-, and
Objective-C-inspired syntax, with **AOT compilation via LLVM**, **deterministic
ARC** memory management, and **built-in async/await concurrency**.

```swift
@main
func main() {
    print("Hello, SukiCode!")
}
```

---

## Features (implemented in alpha.0)

### Core Language
- Swift/TypeScript/Objective-C-inspired syntax
- **AOT compilation** via the LLVM backend (no external assembler/linker required)
- **Deterministic ARC** memory management with `weak` / `unowned` references
- **Move semantics** via `Owned<T>` for performance-critical code
- Built-in **async/await**, **Actor**, **Channel**, and structured concurrency
- **C interop** via `extern` / `foreign` declarations
- Cross-compilation via `--target` (host + selected foreign targets)
- Printing (spec §12.1): `print` appends a trailing newline, `printsl`
  ("single-line") does not

### Type System
- Value types: `struct`, `enum` (copied on assignment)
- Reference types: `class`, `actor` (heap, ARC-managed)
- Optional types `T?` with optional chaining `?.`, force unwrap `!`, nil coalescing `??`
- Generic functions and types with `where` constraints, `associatedtype`, `some`
- Protocols (properties, methods, subscripts, initializers)
- Extensions (methods, computed properties, subscripts), `typealias`

### Control Flow
- `if` / `else if` / `else`, `guard`, `switch` with pattern matching
- `for-in` over arrays and dictionaries (tuple destructuring)
- `while`, `repeat-while`
- `select` statement for channel multiplexing
- `do-catch` error handling; `try` / `try?` / `try!`

> **Note:** `defer` is parsed but **not yet code-generated** — see
> [Known Limitations](#known-limitations).

### Functions and Closures
- External/internal parameter labels, default values, `inout` parameters
- Swift-style closures `{ (params) -> ReturnType in statements }`
- Arrow-style closures `(params) => expression`
- Capture lists `[weak self]`, `[unowned self]`
- Trailing closure syntax

> **Note:** `@autoclosure` is accepted by the parser but has no semantic or
> code-generation support yet.

### Memory Management
- ARC for all `class` and `actor` instances
- `weak` references (auto-nil on deallocation)
- `unowned` references (non-optional, no auto-nil)
- `Owned<T>` for unique ownership with move semantics
- `unsafe` blocks and `UnsafePointer` / `UnsafeMutablePointer` for raw memory

### Concurrency (most complete subsystem)
- `async` / `await` backed by LLVM coroutine intrinsics
- `Task`, `TaskGroup` (`withTaskGroup`), `Channel<T>` (bounded/unbounded, `for await`)
- `select` statement; `actor` isolation (serialized message passing)
- `Atomic<T>`, `Mutex`, `Semaphore`, `Condition`, `Future` / `Promise`

### Error Handling
- `throws` function modifier, `do`-`catch` blocks
- `try?` (→ `nil`), `try!` (crash on error)
- `Error` protocol, `Result<T, E>`

> **Note:** `Error.localizedDescription` is **not** provided yet.

### Macros (partial)
- `@freestanding(expression)` / `@freestanding(declaration)`
- `@attached(member)`
- `#makeExpr` / `#makeDecl` templates with `\(arg)` unquoting
- Hygienic symbol generation via `#unique("base")`
- Sandboxed expansion (no file I/O, network, process, or syscalls)

> **Note:** `@attached(peer/access/conformance/extension)` roles are **not**
> implemented yet.

### C / Foreign Interop
- `extern "C" func ...` and `extern "C" { ... }` blocks
- Variadic parameters with `...`
- Calling conventions: `"C"` (default), `"stdcall"`
- `@_cdecl("name")` to export functions to C

> **Note:** Objective-C interop exists only as the `protocol ObjCObject {}`
> marker; the ObjC runtime bridge (`objc_msgSend`, `#selector`, etc.) is **not**
> implemented.

### Tooling
- **`sukic`** — the compiler: multi-backend emit (`--emit-llvm` / `--emit-ast`
  / object / executable), `--target`, `--incremental`, `-expand-macros`
- **`suki-lsp`** — language server (completion, diagnostics)
- **`suki-doc`** — documentation generator
- **`suki-fmt`** — code formatter (**placeholder**: currently a no-op pass-through)
- **`sukipm`** — package manager: `init` / `build` / `test` work; `publish` / `add`
  are **not** implemented yet

### Standard Library (implemented)
| Module      | Highlights |
|-------------|-----------|
| `core`      | `Array`, `Dictionary`, `Set`, `String`, `Optional` bridging, numeric & string utilities, randomness |
| `system`    | `File`, `Path`, `Date`, `Clock`, `OS`, process & environment access |
| `concurrency` | `Task`, `TaskGroup`, `Channel`, atomics, locks, `Future`/`Promise` |
| `data`      | `Data` byte buffer, `Base64`, `Hex`, … |
| `memory`    | `Owned<T>`, `MemoryPool<T>`, unsafe pointer helpers |
| `test`      | `TestFramework` for `sukipm test` |

> **Not yet present:** `network`, `crypto`, `i18n`, `cli`, `SukiUI` modules
> described in the specification.

---

## Quick Start

### Option A — Download a prebuilt toolchain (recommended)

Every tagged release publishes ready-to-use artifacts on the
[GitHub Releases page](https://github.com/bil-fis/SukiCode/releases):

| Artifact | Contents |
|----------|----------|
| `sukicode-linux-x86_64.zip` | `bin/sukic` + `bin/suki` launcher, prebuilt runtime object, standard-library sources, bundled `libLLVM` |
| `sukicode-windows-x86_64.zip` | Same layout with `bin/sukic.exe` + `bin/suki.bat` (LLVM is linked statically) |
| `sukicode-vscode-<version>.vsix` | VS Code extension — syntax highlighting (LSP client wiring is prepared but not yet active) |

```bash
unzip sukicode-linux-x86_64.zip
cd sukicode-linux-x86_64
./bin/suki run ~/hello.suki
```

The `suki` launcher points the compiler at the bundled runtime and standard
library, so the archive works from **any** extraction directory. The only
external requirement is **`clang` on `PATH`** (used for the link stage).

For VS Code: *Extensions → Install from VSIX…* and pick the `.vsix` file.

### Option B — Build from source

#### Prerequisites
- CMake ≥ 3.20
- C++20 compiler (MSVC 2022, GCC 12+, or Clang 15+)
- LLVM ≥ 15.0 with development headers (provides `llvm-config`)
- `clang` on `PATH` — the `sukic` driver invokes it for the link stage

No third-party C++ libraries are linked today: the tree contains **no
`find_package` dependency**, so **vcpkg is not required** to build
(`vcpkg.json` exists, but no dependency declared there is currently consumed).

**Installing LLVM**

- Ubuntu/Debian: `sudo apt install llvm-dev clang lld`
- macOS (Homebrew): `brew install llvm`
- Windows: download the release installer from
  [llvm/llvm-project releases](https://github.com/llvm/llvm-project/releases)
  (install to the default `C:\Program Files\LLVM`)

### Building

```bash
git clone https://github.com/SukiCode/SukiCode.git
cd SukiCode

# Configure
cmake -B build -DCMAKE_BUILD_TYPE=Release

# Build the compiler and tools
cmake --build build -j
```

### Running the tests

```bash
# Builds (if needed) then runs the full suite
./run_all_tests.sh
# Expected: 92 passing tests, 0 failures,
#           110 .suki files parse successfully.
```

### Using the compiler

```bash
# Compile and run
./build/bin/sukic run examples/hello_world.suki

# Compile to an executable
./build/bin/sukic -o hello examples/hello_world.suki
./hello

# Inspect intermediate representations
./build/bin/sukic --emit-llvm examples/hello_world.suki
./build/bin/sukic --emit-ast  examples/hello_world.suki

# Run the concurrency example
./build/bin/sukic run examples/concurrency.suki
```

---

## Language Overview (verified examples)

### Variables and Types
```swift
var name: String = "SukiCode"
let version = 1.0
let count: Int = 42
let optional: String? = nil
```

### Functions
```swift
func greet(person name: String, from city: String = "Unknown") -> String {
    return "Hello \(name) from \(city)"
}

// Generic function
func identity<T>(_ value: T) -> T {
    return value
}
```

### Structs, Classes, Enums, Protocols
```swift
struct Point {
    var x, y: Double
    var magnitude: Double {
        get { return (x * x + y * y).squareRoot() }
    }
}

class Animal {
    var name: String
    init(name: String) { self.name = name }
    func speak() -> String { return "..." }
    deinit { /* cleanup */ }
}

enum Direction { case north, south, east, west }

enum Result<T, E: Error> {
    case success(T)
    case failure(E)
}

protocol Drawable {
    func draw()
    var color: String { get set }
}
```

### Optional Chaining
```swift
let name: String? = "Suki"
let upper = name?.uppercased()       // Optional chaining
let safe  = name ?? "default"        // Nil coalescing
```

### Concurrency
```swift
import concurrency

// Channel + Task + for-await
async func producerConsumer() {
    let ch = Channel<Int>(capacity: 10)
    Task {
        await ch.send(1)
        await ch.close()
    }
    for await value in ch {
        print("got \(value)")
    }
}

// Structured concurrency with TaskGroup
async func fanOut() {
    await withTaskGroup { group in
        group.addTask { return 42 }
        for await r in group {
            print("result \(r)")
        }
    }
}

actor Counter {
    private var value = 0
    func increment() { value += 1 }
    func getValue() -> Int { return value }
}
```

### Error Handling
```swift
enum MyError: Error { case notFound }

func risky() throws -> Int { throw MyError.notFound }

do {
    let result = try risky()
} catch {
    print("Error: \(error)")
}
```

### C Interop
```swift
extern "C" {
    func printf(_ fmt: UnsafePointer<Int8>, _ args: UnsafePointer<Void>...) -> Int32
}

let rc = printf("Hello %s\n", "World")
```

### Macros
```swift
@freestanding(expression)
macro doubleValue(x: Int) -> Int {
    return #makeExpr("(\(x) * 2)")
}

let doubled = #doubleValue(x: 21)   // expands to (21 * 2)
```

---

## Project Structure

```
SukiCode/
├── CMakeLists.txt          # Top-level CMake configuration
├── CMakePresets.json
├── vcpkg.json              # Dependency manifest (kept for future use; nothing is consumed today)
├── .github/
│   ├── workflows/ci.yml    # GitHub Actions: push → tests, v* tag → release artifacts
│   └── scripts/            # package_linux.sh / package_windows.ps1 (standalone archives)
├── idePlugins/vscode/      # VS Code extension (syntax highlighting; LSP wiring pending)
├── src/
│   ├── compiler/           # Compiler (sukic)
│   │   ├── lexer/          #   Tokenizer
│   │   ├── parser/         #   Recursive-descent parser
│   │   ├── ast/            #   Abstract syntax tree
│   │   ├── sema/           #   Semantic analysis
│   │   ├── codegen/        #   LLVM IR generation
│   │   ├── macro/          #   Macro expansion engine
│   │   └── diag/           #   Diagnostics
│   ├── runtime/            # Runtime library (ARC, coroutines, concurrency, pool)
│   └── stdlib/             # Standard library (SukiCode sources)
│       ├── core/           #   Array, Dictionary, Set, String, utilities
│       ├── system/         #   File, Path, Date, Clock, OS, process
│       ├── concurrency/    #   Task, TaskGroup, Channel, atomics, locks
│       ├── data/           #   Data, Base64, Hex
│       ├── memory/         #   Owned<T>, MemoryPool, unsafe helpers
│       └── test/           #   TestFramework
├── tools/
│   ├── suki-fmt/           # Code formatter (placeholder)
│   ├── suki-lsp/           # Language server
│   ├── sukipm/             # Package manager (init/build/test)
│   └── suki-doc/           # Documentation generator
├── moduleTest/             # SukiCode integration tests (98 .suki files)
├── examples/               # Example programs (plus the hellosuki.suki showcase)
├── tests/                  # C++ unit tests
├── run_all_tests.sh        # Test runner
├── SukiCode_Specification.md
├── CHANGELOG.md
├── NOTICE
├── CONTRIBUTING.md
└── LICENSE / LICENSES/
```

---

## Known Limitations

The following are **explicitly not implemented** in `v0.0.0-alpha.0.20261007`
and should not be relied upon:

- **`defer`** — parsed but the body is not emitted (statements are dropped).
- **`#if os(...)` / `#if arch(...)` / `-D` conditional compilation** — not implemented.
- **`@autoclosure`** — parsed only; no semantic/codegen support.
- **`@attached(peer|access|conformance|extension)`** macros — only `member` works.
- **`Error.localizedDescription`** — not provided.
- **System programming** — `MMapRegion`, `DynamicLibrary`, and a `sys` module
  (fork/exec/socket/signal) are **not** present; `asm(...)` and `MemoryLayout<T>`
  are implemented.
- **Named `ThreadPool` type** — the underlying thread APIs exist, but there is no
  `ThreadPool` / `DispatchQueue` type. Use `Task` / `TaskGroup` instead.
- **Objective-C runtime bridge** — only the `ObjCObject` marker protocol exists.
- **Standard library** — `network`, `crypto`, `i18n`, `cli`, `SukiUI` modules are
  **not** present.
- **`suki-fmt`** — implemented as a pass-through placeholder (does not reformat).
- **SukiPM `publish` / `add`** — not implemented.

---

## License

SukiCode uses multiple licenses for different components:

- **Compiler, Runtime, Standard Library**: Apache-2.0 **WITH** Runtime Library Exception
- **Tools** (`sukipm`, `suki-lsp`, `suki-fmt`, `suki-doc`): MIT
- **Language Specification**: CC BY 4.0
- **Examples and Tests**: MIT

**Programs written in SukiCode can use any license, including proprietary and
closed-source licenses** (the Runtime Library Exception removes copyleft
obligation from compiled programs).

See [LICENSE](LICENSE), [LICENSES/](LICENSES/), [NOTICE](NOTICE), and
[README_LICENSE.md](README_LICENSE.md) for details.

---

*Designed by 林晚晚ss, 2026. SukiCode is a pre-release project; APIs may change
before the 1.0 stable release.*
