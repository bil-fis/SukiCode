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

    // 架构规范化名（规范 §10.3 列出 x86_64 / arm64 / arm / riscv64）。
    auto setArch = [&](const char* name) { out.arch = name; };

    if (arch == "x86_64" || arch == "amd64") {
        out.pointerWidth = 64; out.pointerAlign = 8;
        setArch("x86_64");
    } else if (arch == "aarch64" || arch == "arm64") {
        out.pointerWidth = 64; out.pointerAlign = 8;
        setArch("arm64");
    } else if (arch == "riscv64") {
        out.pointerWidth = 64; out.pointerAlign = 8;
        setArch("riscv64");
    } else if (arch == "i386" || arch == "i686" || arch == "x86") {
        out.pointerWidth = 32; out.pointerAlign = 4;
        setArch("i386");
    } else if (arch == "armv7" || arch == "arm") {
        out.pointerWidth = 32; out.pointerAlign = 4;
        setArch("arm");
    } else if (arch == "wasm32") {
        out.pointerWidth = 32; out.pointerAlign = 4;
        out.isWasm = true; out.format = ObjectFormat::Wasm;
        out.allocAlignment = 16;
        setArch("wasm32");
        out.os = "WASI";
        return true;
    } else if (arch == "wasm64") {
        out.pointerWidth = 64; out.pointerAlign = 8;
        out.isWasm = true; out.format = ObjectFormat::Wasm;
        setArch("wasm64");
        out.os = "WASI";
        return true;
    } else {
        return false;
    }

    // Object format / OS detection from the triple body.
    // `os` 同时用于 §10.3 的 `os(...)` 谓词；比对顺序按“更具体者优先”
    // 排列（iOS 在 macOS 之前，因为 iOS 三元组同样含 apple）。
    if (triple.find("windows") != std::string::npos ||
        triple.find("msvc") != std::string::npos ||
        triple.find("mingw") != std::string::npos) {
        out.format = ObjectFormat::COFF;
        out.os = "Windows";
    } else if (triple.find("ios") != std::string::npos) {
        out.format = ObjectFormat::MachO;
        out.os = "iOS";
    } else if (triple.find("darwin") != std::string::npos ||
               triple.find("apple") != std::string::npos ||
               triple.find("macos") != std::string::npos) {
        out.format = ObjectFormat::MachO;
        out.os = "macOS";
    } else if (triple.find("android") != std::string::npos) {
        out.format = ObjectFormat::ELF;
        out.os = "Android";
    } else if (triple.find("freebsd") != std::string::npos) {
        out.format = ObjectFormat::ELF;
        out.os = "FreeBSD";
    } else if (triple.find("wasi") != std::string::npos) {
        out.format = ObjectFormat::Wasm;
        out.os = "WASI";
    } else {
        out.format = ObjectFormat::ELF;
        out.os = "Linux";
    }

    // Endianness: s390x and some bare-metal MIPS are big-endian.
    if (arch == "s390x" || arch == "mips" || arch == "mips64" ||
        arch == "powerpc" || arch == "powerpc64") {
        out.endian = Endian::Big;
    }
    return true;
}

TargetInfo hostTarget() {
    // 按宿主操作系统 + 架构推导默认目标，使编译器在 Windows / macOS / Linux
    // 上原生构建时都能正确选择目标三元组（规范 §10.3 与代码生成的默认值）。
#if defined(_WIN32) || defined(_WIN64)
    #if defined(_M_ARM64) || defined(__aarch64__)
        TargetInfo t; getTargetInfo("aarch64-pc-windows-msvc", t); return t;
    #else
        TargetInfo t; getTargetInfo(triples::X86_64Windows, t); return t;
    #endif
#elif defined(__APPLE__)
    #if defined(__x86_64__)
        TargetInfo t; getTargetInfo(triples::X86_64Darwin, t); return t;
    #else
        TargetInfo t; getTargetInfo(triples::AArch64Darwin, t); return t;
    #endif
#elif defined(__x86_64__)
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

std::vector<std::string> listKnownTargets() {
    // 规范 §14.1.1 表格中的代表性目标三元组。
    return {
        // 桌面系统
        "x86_64-unknown-linux-gnu",
        "aarch64-unknown-linux-gnu",
        "riscv64-unknown-linux-gnu",
        "x86_64-pc-windows-msvc",
        "x86_64-pc-windows-gnu",
        "aarch64-pc-windows-msvc",
        "x86_64-apple-darwin",
        "arm64-apple-darwin",
        // 移动端
        "armv7-linux-androideabi",
        "aarch64-linux-android",
        // 嵌入式 Linux
        "arm-linux-gnueabihf",
        "riscv64-linux-gnu",
        // 裸机 / 单片机
        "thumbv7m-none-eabi",
        "riscv32imac-unknown-none-elf",
        // 实时操作系统
        "armv7-rtems",
        "riscv64-rtems",
        // Web 浏览器
        "wasm32-unknown-unknown",
        "wasm32-unknown-wasi",
        // 裸机（规范 §8.7）：禁用标准库、仅用 core 模块的约定目标名
        "bare-metal",
    };
}

} // namespace suki
