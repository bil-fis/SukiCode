#include "compiler/codegen/TargetInfo.h"

namespace suki {

bool getTargetInfo(const std::string& triple, TargetInfo& out) {
    out = TargetInfo{};
    out.triple = triple;
    if (triple.empty()) return false;

    // Architecture prefix determines pointer width and endianness; the OS part
    // determines the object format. We intentionally keep this table small and
    // default to a sane ELF/Little/64-bit target for anything unrecognised so
    // the frontend can still make progress; the IR generator still hands the
    // exact triple to LLVM, which is the authority for actual code emission.
    const std::string arch = triple.substr(0, triple.find('-'));

    if (arch == "x86_64" || arch == "amd64") {
        out.pointerWidth = 64; out.pointerAlign = 8;
    } else if (arch == "aarch64" || arch == "arm64") {
        out.pointerWidth = 64; out.pointerAlign = 8;
    } else if (arch == "riscv64") {
        out.pointerWidth = 64; out.pointerAlign = 8;
    } else if (arch == "i386" || arch == "i686" || arch == "x86") {
        out.pointerWidth = 32; out.pointerAlign = 4;
    } else if (arch == "armv7" || arch == "arm") {
        out.pointerWidth = 32; out.pointerAlign = 4;
    } else if (arch == "wasm32") {
        out.pointerWidth = 32; out.pointerAlign = 4;
        out.isWasm = true; out.format = ObjectFormat::Wasm;
        out.allocAlignment = 16;
        return true;
    } else if (arch == "wasm64") {
        out.pointerWidth = 64; out.pointerAlign = 8;
        out.isWasm = true; out.format = ObjectFormat::Wasm;
        return true;
    } else {
        return false;
    }

    // Object format / OS detection from the triple body.
    if (triple.find("windows") != std::string::npos) {
        out.format = ObjectFormat::COFF;
    } else if (triple.find("darwin") != std::string::npos ||
               triple.find("apple") != std::string::npos ||
               triple.find("macos") != std::string::npos) {
        out.format = ObjectFormat::MachO;
    } else {
        out.format = ObjectFormat::ELF;
    }

    // Endianness: s390x and some bare-metal MIPS are big-endian.
    if (arch == "s390x" || arch == "mips" || arch == "mips64" ||
        arch == "powerpc" || arch == "powerpc64") {
        out.endian = Endian::Big;
    }
    return true;
}

TargetInfo hostTarget() {
#if defined(__x86_64__)
    TargetInfo t; getTargetInfo(triples::X86_64Linux, t); return t;
#elif defined(__aarch64__)
    TargetInfo t; getTargetInfo(triples::AArch64Linux, t); return t;
#elif defined(__riscv)
    TargetInfo t; getTargetInfo(triples::RISCV64Linux, t); return t;
#elif defined(__i386__)
    TargetInfo t; getTargetInfo("i386-unknown-linux-gnu", t); return t;
#else
    TargetInfo t; getTargetInfo(triples::X86_64Linux, t); return t;
#endif
}

} // namespace suki
