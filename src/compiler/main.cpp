// suki.cpp — SukiCode compiler driver (sukic)
// Entry point for the SukiCode compiler.
//
// Usage:
//   sukic [options] <input.suki>
//   sukic build           — build via SukiPM
//   sukic run             — build and run
//
// Options:
//   -o <file>             — output file
//   -c                    — compile only (no link)
//   -S                    — output assembly
//   --emit-llvm           — output LLVM IR
//   --emit-ast            — output AST
//   --target=<triple>     — target triple
//   --sysroot=<path>      — system root
//   -O<level>             — optimization level (0-3, s, z)
//   -g                    — debug info
//   -D<flag>              — define compilation flag
//   --incremental         — enable incremental compilation
//   --list-targets        — list supported targets
//   -v, --verbose         — verbose output
//   --version             — print version
//   -h, --help            — show help

#include "compiler/lexer/Lexer.h"
#include "compiler/lexer/Token.h"
#include "compiler/parser/Parser.h"
#include "compiler/ast/ASTPrinter.h"
#include "compiler/sema/Sema.h"
#include "compiler/codegen/IRGenerator.h"
#include "compiler/codegen/ObjectEmitter.h"
#include "compiler/diag/Diagnostic.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <filesystem>

namespace fs = std::filesystem;

static void printVersion() {
    std::cout << "sukic (SukiCode compiler) 0.1.0\n";
    std::cout << "Part of the SukiCode toolchain\n";
}

static void printHelp() {
    std::cout << R"(SukiCode Compiler (sukic) — Usage:

  sukic [options] <input.suki>
  sukic build           Build project via SukiPM
  sukic run             Build and run

Options:
  -o <file>             Output file
  -c                    Compile only (produce .o)
  -S                    Output assembly
  --emit-llvm           Output LLVM IR (.ll)
  --emit-ast            Output AST dump
  --target=<triple>     Target triple (e.g., x86_64-linux-gnu)
  --sysroot=<path>      System root for cross-compilation
  -O<level>             Optimization: 0, 1, 2, 3, s, z
  -g                    Include debug info
  -D<flag>              Define a compilation flag
  --incremental         Enable incremental compilation
  --list-targets        List supported targets
  -v, --verbose         Verbose output
  --version             Print version
  -h, --help            Show this help
)";
}

static void printTargets() {
    std::cout << R"(Supported targets:
  Desktop:
    x86_64-linux-gnu          x86_64 Linux (glibc)
    x86_64-linux-musl         x86_64 Linux (musl)
    aarch64-linux-gnu         ARM64 Linux
    x86_64-windows-gnu        x86_64 Windows (MinGW)
    x86_64-windows-msvc       x86_64 Windows (MSVC)
    aarch64-apple-darwin      ARM64 macOS
    x86_64-apple-darwin       x86_64 macOS

  Mobile:
    armv7-linux-android       ARM Android
    aarch64-linux-android     ARM64 Android
    aarch64-apple-ios         ARM64 iOS

  Embedded / Bare-metal:
    thumbv7m-none-eabi        ARM Cortex-M
    thumbv7em-none-eabihf     ARM Cortex-M with FPU
    riscv32imac-unknown-none-elf  RISC-V 32
    riscv64-linux-gnu         RISC-V 64 Linux

  WebAssembly:
    wasm32-unknown-unknown    WebAssembly
)";
}

// Command-line options
struct Options {
    std::string inputFile;
    std::string outputFile;
    bool compileOnly = false;
    bool emitAssembly = false;
    bool emitLLVM = false;
    bool emitAST = false;
    std::string target;
    std::string sysroot;
    int optLevel = 0;
    bool debugInfo = false;
    bool incremental = false;
    bool verbose = false;
    std::vector<std::string> defines;
};

static bool parseArgs(int argc, char* argv[], Options& opts) {
    if (argc < 2) {
        printHelp();
        return false;
    }

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (arg == "-h" || arg == "--help") {
            printHelp();
            return false;
        } else if (arg == "--version") {
            printVersion();
            return false;
        } else if (arg == "--list-targets") {
            printTargets();
            return false;
        } else if (arg == "-c") {
            opts.compileOnly = true;
        } else if (arg == "-S") {
            opts.emitAssembly = true;
        } else if (arg == "--emit-llvm") {
            opts.emitLLVM = true;
        } else if (arg == "--emit-ast") {
            opts.emitAST = true;
        } else if (arg == "-g") {
            opts.debugInfo = true;
        } else if (arg == "--incremental") {
            opts.incremental = true;
        } else if (arg == "-v" || arg == "--verbose") {
            opts.verbose = true;
        } else if (arg.substr(0, 2) == "-o" && arg.size() > 2) {
            opts.outputFile = arg.substr(2);
        } else if (arg == "-o") {
            if (i + 1 < argc) {
                opts.outputFile = argv[++i];
            }
        } else if (arg.substr(0, 2) == "-O") {
            if (arg.size() > 2) {
                char level = arg[2];
                if (level >= '0' && level <= '3') opts.optLevel = level - '0';
                else if (level == 's') opts.optLevel = 4;
                else if (level == 'z') opts.optLevel = 5;
            }
        } else if (arg.substr(0, 2) == "-D") {
            opts.defines.push_back(arg.substr(2));
        } else if (arg.substr(0, 9) == "--target=") {
            opts.target = arg.substr(9);
        } else if (arg.substr(0, 10) == "--sysroot=") {
            opts.sysroot = arg.substr(10);
        } else if (arg[0] != '-') {
            opts.inputFile = arg;
        } else {
            std::cerr << "sukic: unknown option: " << arg << "\n";
            return false;
        }
    }

    return true;
}

static std::string readFile(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::cerr << "sukic: cannot open file: " << path << "\n";
        return "";
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

int main(int argc, char* argv[]) {
    Options opts;
    if (!parseArgs(argc, argv, opts)) {
        return 1;
    }

    if (opts.inputFile.empty()) {
        std::cerr << "sukic: no input file\n";
        return 1;
    }

    // Handle special commands
    if (opts.inputFile == "build") {
        std::cerr << "sukic: 'build' command requires SukiPM (not yet implemented)\n";
        return 1;
    }
    if (opts.inputFile == "run") {
        std::cerr << "sukic: 'run' command requires SukiPM (not yet implemented)\n";
        return 1;
    }

    // Read source file
    std::string source = readFile(opts.inputFile);
    if (source.empty()) return 1;

    if (opts.verbose) {
        std::cerr << "sukic: compiling " << opts.inputFile << "\n";
    }

    // Initialize diagnostic engine
    suki::DiagnosticEngine diag;

    // ─── Phase 1: Lexing ──────────────────────────────────────────────────
    if (opts.verbose) std::cerr << "sukic: lexing...\n";

    suki::Lexer lexer(source, opts.inputFile, diag);
    lexer.setDefines(opts.defines); // 传递 -D 定义到词法分析器
    auto tokens = lexer.lexAll();

    if (diag.hadErrors()) {
        diag.printAll(source, opts.inputFile);
        return 1;
    }

    if (opts.verbose) {
        std::cerr << "sukic: lexed " << tokens.size() << " tokens\n";
    }

    // ─── Phase 2: Parsing ─────────────────────────────────────────────────
    if (opts.verbose) std::cerr << "sukic: parsing...\n";

    suki::Parser parser(std::move(tokens), source, opts.inputFile, diag);
    auto ast = parser.parse();

    if (diag.hadErrors()) {
        diag.printAll(source, opts.inputFile);
        return 1;
    }

    if (opts.verbose) {
        std::cerr << "sukic: parsed " << ast->declarations.size() << " declarations\n";
    }

    // ─── Emit AST ─────────────────────────────────────────────────────────
    if (opts.emitAST) {
        suki::ASTPrinter printer;
        std::cout << printer.print(*ast);
        return 0;
    }

    // ─── Phase 3: Semantic Analysis ───────────────────────────────────────
    if (opts.verbose) std::cerr << "sukic: semantic analysis...\n";

    suki::Sema sema(diag);
    if (!sema.analyze(*ast)) {
        diag.printAll(source, opts.inputFile);
        return 1;
    }

    // ─── Phase 4: IR Generation ───────────────────────────────────────────
    if (opts.verbose) std::cerr << "sukic: generating IR...\n";

    std::string moduleName = fs::path(opts.inputFile).stem().string();
    suki::IRGenerator codegen(diag, moduleName);
    codegen.setEmitDebugInfo(opts.debugInfo);

    if (!codegen.generate(*ast)) {
        diag.printAll(source, opts.inputFile);
        return 1;
    }

    // ─── Emit LLVM IR ─────────────────────────────────────────────────────
    if (opts.emitLLVM) {
        std::cout << codegen.getIRString();
        return 0;
    }

    // ─── Phase 5: Object code generation (requires LLVM) ──────────────────
#ifdef SUKI_HAS_LLVM
    suki::ObjectEmitter::initializeTargets();

    // 确定输出路径 / Determine output path
    std::string outputPath;
    if (!opts.outputFile.empty()) {
        outputPath = opts.outputFile;
    } else {
        outputPath = moduleName + ".o";
    }

    // 输出汇编文件 / Emit assembly
    if (opts.emitAssembly) {
        std::string asmPath = outputPath;
        if (asmPath.substr(asmPath.size() - 2) != ".s") {
            asmPath = moduleName + ".s";
        }
        if (opts.verbose) std::cerr << "sukic: emitting assembly to " << asmPath << "\n";
        auto llvmModule = codegen.releaseModule();
        suki::ObjectEmitter emitter;
        if (!emitter.emitAssembly(*llvmModule, asmPath, opts.target)) {
            diag.printAll(source, opts.inputFile);
            return 1;
        }
        std::cerr << "sukic: assembly written to " << asmPath << "\n";
        return 0;
    }

    // 输出目标文件 / Emit object file
    if (opts.compileOnly || outputPath.substr(outputPath.size() - 2) == ".o" ||
        outputPath.substr(outputPath.size() - 4) == ".obj") {
        if (opts.verbose) std::cerr << "sukic: emitting object file to " << outputPath << "\n";
        auto llvmModule = codegen.releaseModule();
        suki::ObjectEmitter emitter;
        if (!emitter.emitObjectFile(*llvmModule, outputPath, opts.target)) {
            diag.printAll(source, opts.inputFile);
            return 1;
        }
        std::cerr << "sukic: object file written to " << outputPath << "\n";
        return 0;
    }

    // 链接可执行文件 / Link executable
    {
        // 先输出临时 .obj 文件
        std::string objPath = outputPath + ".obj";
        if (opts.verbose) std::cerr << "sukic: generating object file...\n";
        auto llvmModule = codegen.releaseModule();
        suki::ObjectEmitter emitter;
        if (!emitter.emitObjectFile(*llvmModule, objPath, opts.target)) {
            diag.printAll(source, opts.inputFile);
            return 1;
        }

        // 链接
        if (opts.verbose) std::cerr << "sukic: linking " << outputPath << "...\n";
        std::vector<std::string> objFiles = {objPath};
        if (!suki::ObjectEmitter::linkExecutable(objFiles, outputPath, opts.target)) {
            std::cerr << "sukic: linking failed\n";
            return 1;
        }

        // 清理临时 .o 文件 / Cleanup temporary .o file
        std::remove(objPath.c_str());

        std::cerr << "sukic: executable written to " << outputPath << "\n";
        return 0;
    }
#else
    // 无 LLVM 后端 / No LLVM backend
    if (opts.verbose) {
        std::cerr << "sukic: compilation successful (no LLVM backend)\n";
    }
    if (!opts.outputFile.empty() && opts.emitLLVM) {
        std::ofstream out(opts.outputFile);
        out << codegen.getIRString();
    }
    if (diag.hadErrors()) {
        diag.printAll(source, opts.inputFile);
        return 1;
    }
    return 0;
#endif
}
