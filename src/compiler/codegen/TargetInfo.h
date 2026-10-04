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
