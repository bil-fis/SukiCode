> ***<span style="color:red">Warning:</span>*** **This language is under developement and co-worked with Xiaomi MiMo V2.5-pro**  
> Now it currently not work properly, and i cannot fix it now XD  
> So that's it.

# SukiCode

A modern systems programming language that fuses Swift, TypeScript, and Objective-C syntax, with AOT compilation via LLVM, deterministic ARC memory management, and built-in async/await concurrency.

## Features

### Core Language
- **Swift-influenced syntax**: Clean, expressive, and safe
- **AOT compilation** via LLVM backend — no external assembler/linker needed
- **Deterministic ARC** memory management with Pool/Collection resource abstractions
- **Built-in async/await**, Actor, Channel, and structured concurrency
- **Move semantics** via `Owned<T>` for performance-critical code
- **C/Objective-C interop** via `extern` declarations
- **Cross-platform**: Windows, Linux, macOS, Android, iOS, bare-metal (ARM/RISC-V), WebAssembly

### Type System
- Value types: `struct`, `enum` (stack/inline, copied on assignment)
- Reference types: `class`, `actor` (heap, ARC managed)
- Optional types `Type?` with nil, optional chaining `?.`, force unwrap `!`, nil coalescing `??`
- Generic functions and types with protocol constraints
- Protocols with required properties, methods, subscripts, initializers
- Extensions for adding methods, computed properties, subscripts
- Type aliases with generic parameters

### Control Flow
- `if`/`else if`/`else`, `guard`, `switch` with pattern matching
- `for-in` loops over arrays and dictionaries (tuple destructuring)
- `while`, `repeat-while`, `defer`, `do-catch` error handling
- `select` statement for channel multiplexing

### Functions and Closures
- External/internal parameter labels, default values, `inout` parameters
- Swift-style closures `{ (params) -> ReturnType in statements }`
- Arrow-style closures `(params) => expression`
- Capture lists `[weak self]`, `[unowned self]`
- Trailing closure syntax, `@autoclosure`

### Memory Management
- ARC for all class and actor instances
- `weak` references (auto-nil on deallocation)
- `unowned` references (non-optional, no auto-nil)
- Pool/Collection resource lifecycle management
- `Owned<T>` for unique ownership with move semantics

### Concurrency
- `ThreadPool` with work-stealing scheduling
- `async`/`await` with LLVM coroutine intrinsics
- `TaskGroup` for structured concurrency
- `Channel<T>` with bounded/unbounded, 4 backpressure policies
- `select` statement for channel multiplexing
- `Atomic<T>`, `Mutex`, `RWLock`, `Semaphore`, `Condition`
- `DispatchQueue` (serial/parallel)

### System Programming
- Inline assembly via `asm(...)` keyword
- `MMapRegion` for memory-mapped files
- `DynamicLibrary` for runtime `.so`/`.dylib`/`.dll` loading
- `sys` module for OS primitives (fork, exec, socket, signal, etc.)
- `MemoryLayout<T>` for querying size, alignment, stride
- `unsafe` blocks for raw pointer operations

### Error Handling
- `throws` function modifier, `do`-`catch` blocks
- `try?` (converts to nil), `try!` (crashes on error)
- `Error` protocol with `localizedDescription`
- `Result<T, E>` for non-throwing async scenarios

### Macros
- `@macro` for compile-time metaprogramming
- `@freestanding` and `@attached` macro kinds
- Sandboxed execution (no file I/O, network, process, syscall)
- Hygienic symbol generation with `#unique("base")`

### C/Objective-C Interop
- `extern "C" func` for individual function declarations
- `extern "C" { }` blocks for multiple declarations
- Variadic parameters with `...`
- Calling conventions: `"C"` (default), `"stdcall"`
- ObjC runtime functions auto-pre-declared (`objc_msgSend`, `sel_registerName`, etc.)
- `@_cdecl("name")` attribute to export functions to C

## Quick Start

### Prerequisites

- CMake ≥ 3.20
- C++20 compiler (MSVC 2022, GCC 12+, Clang 15+)
- LLVM ≥ 15.0 (with development headers)

#### Installing LLVM

**Ubuntu/Debian:**
```bash
sudo apt install llvm-dev clang-dev lld
```

**macOS (Homebrew):**
```bash
brew install llvm
```

**Windows:**
Download from [releases.llvm.org](https://releases.llvm.org/) or use vcpkg:
```bash
vcpkg install llvm
```

### Building

```bash
# Clone
git clone https://github.com/your-org/SukiCode.git
cd SukiCode

# Configure
cmake -B build -DCMAKE_BUILD_TYPE=Debug

# Build
cmake --build build

# Run tests
cd build && ctest
```

### Using the compiler

```bash
# Compile a SukiCode file
./build/bin/sukic -o hello examples/hello_world.suki

# Emit LLVM IR
./build/bin/sukic --emit-llvm examples/hello_world.suki

# Dump AST
./build/bin/sukic --emit-ast examples/hello_world.suki
```

## Project Structure

```
SukiCode/
├── CMakeLists.txt          # Top-level CMake configuration
├── src/
│   ├── compiler/           # Compiler (sukic)
│   │   ├── lexer/          # Tokenizer
│   │   ├── parser/         # Recursive descent parser
│   │   ├── ast/            # Abstract Syntax Tree
│   │   ├── sema/           # Semantic analysis
│   │   ├── codegen/        # LLVM IR generation
│   │   ├── macro/          # Macro expansion engine
│   │   └── diag/           # Diagnostics
│   ├── runtime/            # Runtime library
│   │   ├── arc/            # ARC memory management
│   │   ├── pool/           # Pool/Collection
│   │   └── concurrency/    # Thread pool, channels, coroutines
│   └── stdlib/             # Standard library (C++ headers)
│       ├── core/           # Array, Dictionary, Set, String, Optional, etc.
│       ├── system/         # Process, File, Path, Date, MemoryLayout, etc.
│       ├── concurrency/    # DispatchQueue, TaskGroup
│       ├── network/        # URL, URLSession, Socket, JSON
│       ├── crypto/         # SHA256, MD5, HMAC, AES, ChaCha20, RSA, Ed25519
│       ├── data/           # Data, Base64, Hex, GZip, Zlib, XML, CSV, MessagePack
│       ├── i18n/           # Localization, number/date/currency formatting
│       ├── test/           # TestFramework
│       └── cli/            # ArgumentParser
├── tests/
│   ├── unit/               # C++ unit tests
│   └── CMakeLists.txt      # Test configuration
├── moduleTest/             # SukiCode integration tests
│   ├── lexer/              # Lexer tests
│   ├── parser/             # Parser tests
│   ├── codegen/            # Codegen tests
│   ├── integration/        # Integration tests (compile + run)
│   └── run_tests.bat/sh    # Test runner scripts
├── tools/
│   ├── suki-fmt/           # Code formatter
│   ├── suki-lsp/           # Language server
│   ├── sukipm/             # Package manager
│   └── suki-doc/           # Documentation generator
├── examples/               # Example SukiCode programs
└── docs/                   # Documentation
```

## Language Overview

### Hello World
```swift
@main
func main() {
    print("Hello, SukiCode!")
}
```

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

// Async function
async func fetchData() throws -> Data {
    let data = await httpClient.get(url)
    return data
}
```

### Structs and Classes
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
    func speak() -> String { "..." }
    deinit { /* cleanup */ }
}
```

### Enums
```swift
enum Result<T, E: Error> {
    case success(T)
    case failure(E)
}

enum Direction {
    case north, south, east, west
}
```

### Protocols
```swift
protocol Drawable {
    func draw()
    var color: String { get set }
}

struct Circle: Drawable {
    var color: String
    func draw() { /* ... */ }
}
```

### Async/Await and Channels
```swift
async func processData() throws -> Result {
    let ch = Channel<Int>(capacity: 10)
    await ch.send(42)
    let value = await ch.receive()
    return value
}

actor Counter {
    private var value = 0
    func increment() { value += 1 }
    func getValue() -> Int { return value }
}
```

### Error Handling
```swift
enum MyError: Error {
    case notFound
    case permissionDenied
}

func risky() throws -> Int {
    throw MyError.notFound
}

do {
    let result = try risky()
} catch {
    print("Error: \(error)")
}
```

### C/ObjC Interop
```swift
// C function declarations
extern "C" {
    func printf(fmt: UnsafePointer<Int8>, ...) -> Int32
    func malloc(size: UInt64) -> UnsafeMutablePointer<Void>
}

// Usage
let result = printf("Hello %s\n", "World")
```

### Macros
```swift
@macro @freestanding func log(message: String) {
    print("[LOG] \(message)")
}

#log(message: "Hello, Macro!")
```

## License

SukiCode uses multiple licenses for different components:

- **Compiler, Runtime, Standard Library**: Apache 2.0 + Runtime Library Exception
- **Tools** (sukipm, suki-lsp, suki-fmt, suki-doc): MIT
- **Language Specification**: CC BY 4.0
- **Examples and Tests**: MIT

**Programs written in SukiCode can use any license, including proprietary and closed-source licenses.**

See [LICENSE](LICENSE) and [LICENSES/](LICENSES/) for details.

---

*Designed by 林晚晚ss, 2026*
