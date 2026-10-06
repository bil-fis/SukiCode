#pragma once

// Target abstraction for cross-platform code generation.
//
// SukiCode's only code-generation backend is LLVM, which already knows how to
// emit machine code for every supported architecture. This header models the
// small amount of *language-level* target information the frontend and
// runtime need (pointer width, endianness, object-file/ABI flavour, calling
// convention, alignment) independently of LLVM, so the frontend can make
// layout/ABI decisions without pulling in LLVM headers. The IR generator later
// feeds this triple to LLVM's TargetRegistry to select the actual backend.
//
// Keeping this LLVM-free preserves the layering: `TargetInfo` may be included
// by any stage; only the IR generator's implementation sees LLVM.

#include <string>
#include <vector>

namespace suki {

enum class Endian { Little, Big };
enum class ObjectFormat { MachO, ELF, COFF, Wasm };

struct TargetInfo {
    std::string triple;            // e.g. "x86_64-unknown-linux-gnu"
    // 规范 §10.3 条件编译取值：`os(Linux)` / `arch(x86_64)` 等谓词按这两个
    // 规范化名字比对，名字集合取自规范列出的取值（由三元组推导，见
    // getTargetInfo）。未知三元组回退为主机取值，保证前端仍可推进。
    std::string os;                // Linux / macOS / Windows / iOS / Android / FreeBSD / WASI
    std::string arch;              // x86_64 / arm64 / arm / riscv64 / i386 / wasm32 ...
    unsigned pointerWidth = 64;    // bits
    unsigned pointerAlign = 8;     // bytes
    Endian endian = Endian::Little;
    ObjectFormat format = ObjectFormat::ELF;
    bool isWasm = false;           // wasm32 has no native calling convention
    unsigned allocAlignment = 16;  // preferred heap alignment
};

// Look up a target by LLVM-style triple. Returns false for unknown triples.
bool getTargetInfo(const std::string& triple, TargetInfo& out);

// The host target (derived from the build). Always succeeds for the
// platforms the toolchain itself runs on.
TargetInfo hostTarget();

// 规范 §14.1.1：返回 `sukic --list-targets` 展示的已知目标三元组列表
// （桌面 / 移动 / 嵌入式 / 裸机 / RTOS / WebAssembly）。该列表为规范表列的
// 代表性集合；实际可用性仍取决于 LLVM 后端与 `--sysroot` 提供的系统库。
std::vector<std::string> listKnownTargets();

// Well-known triples for the MVP and the cross-platform extension targets.
namespace triples {
inline constexpr const char* X86_64Linux   = "x86_64-unknown-linux-gnu";
inline constexpr const char* AArch64Linux  = "aarch64-unknown-linux-gnu";
inline constexpr const char* RISCV64Linux  = "riscv64-unknown-linux-gnu";
inline constexpr const char* X86_64Darwin  = "x86_64-apple-darwin";
inline constexpr const char* AArch64Darwin = "arm64-apple-darwin";
inline constexpr const char* X86_64Windows = "x86_64-pc-windows-msvc";
inline constexpr const char* Wasm32Wasi    = "wasm32-unknown-wasi";
} // namespace triples

} // namespace suki
