# SukiCode

A modern systems programming language that fuses Swift, TypeScript, and Objective-C syntax, with AOT compilation via LLVM, deterministic ARC memory management, and built-in async/await concurrency.

## Features

- **Swift-influenced syntax**: Clean, expressive, and safe
- **AOT compilation** via LLVM backend — no external assembler/linker needed
- **Deterministic ARC** memory management with Pool/Collection resource abstractions
- **Built-in async/await**, Actor, Channel, and structured concurrency
- **Move semantics** via `Owned<T>` for performance-critical code
- **C/Objective-C interop** via `@cImport`
- **Cross-platform**: Windows, Linux, macOS, Android, iOS, bare-metal (ARM/RISC-V), WebAssembly
- **Package manager** (SukiPM) with GitHub as registry
- **Toolchain**: compiler (`sukic`), package manager (`sukipm`), LSP (`suki-lsp`), formatter (`suki-fmt`)

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
│   │   └── diag/           # Diagnostics
│   ├── runtime/            # Runtime library
│   │   ├── arc/            # ARC memory management
│   │   ├── pool/           # Pool/Collection
│   │   └── concurrency/    # Thread pool, coroutines
│   └── stdlib/             # Standard library (C++ stubs)
│       └── core/           # print(), assert()
├── tests/
│   └── unit/               # Unit tests
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
```

### Functions
```swift
func greet(person name: String, from city: String = "Unknown") -> String {
    return "Hello \(name) from \(city)"
}
```

### Structs and Classes
```swift
struct Point {
    var x, y: Double
}

class Animal {
    var name: String
    init(name: String) { self.name = name }
    func speak() -> String { "..." }
}
```

### Enums
```swift
enum Result<T, E: Error> {
    case success(T)
    case failure(E)
}
```

### Async/Await
```swift
async func fetchData() throws -> Data {
    let data = await httpClient.get(url)
    return data
}
```

### Actors
```swift
actor Counter {
    private var value = 0
    func increment() { value += 1 }
}
```

### Channels
```swift
let ch = Channel<Int>(capacity: 10)
await ch.send(42)
let value = await ch.receive()
```

## License

MIT License — see [LICENSE](LICENSE) for details.

---

*Designed by 林晚晚ss, 2026*
