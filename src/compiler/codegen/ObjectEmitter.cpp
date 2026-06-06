// LLVM 目标代码输出器实现
// Emits object files and links executables.

#include "ObjectEmitter.h"

#ifdef SUKI_HAS_LLVM
#include <llvm/Config/llvm-config.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/TargetParser/Host.h>
#include <llvm/TargetParser/Triple.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Target/TargetOptions.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Transforms/Scalar.h>
#include <llvm/Transforms/Utils.h>
#include <llvm/Transforms/InstCombine/InstCombine.h>
#include <llvm/Transforms/Scalar/GVN.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Transforms/Utils/Cloning.h>
#endif

#include <cstdlib>
#include <iostream>

namespace suki {

ObjectEmitter::ObjectEmitter() = default;
ObjectEmitter::~ObjectEmitter() = default;

void ObjectEmitter::initializeTargets() {
#ifdef SUKI_HAS_LLVM
    // 使用 LLVM 配置宏初始化本机目标 / Use LLVM config macros to initialize native target
    LLVM_NATIVE_TARGETINFO();
    LLVM_NATIVE_TARGET();
    LLVM_NATIVE_TARGETMC();
    LLVM_NATIVE_ASMPARSER();
    LLVM_NATIVE_ASMPRINTER();
#endif
}

#ifdef SUKI_HAS_LLVM

std::string ObjectEmitter::getDefaultTargetTriple() {
    return llvm::sys::getDefaultTargetTriple();
}

bool ObjectEmitter::emitObjectFile(llvm::Module& module, const std::string& outputPath,
                                    const std::string& targetTriple) {
    std::string tripleStr = targetTriple.empty() ? getDefaultTargetTriple() : targetTriple;
    llvm::Triple triple(tripleStr);
    module.setTargetTriple(triple);

    std::string error;
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(tripleStr, error);
    if (!target) {
        std::cerr << "sukic: target not found: " << error << "\n";
        return false;
    }

    llvm::TargetOptions opt;
    auto relocModel = std::optional<llvm::Reloc::Model>();
    llvm::TargetMachine* targetMachine = target->createTargetMachine(
        triple, "generic", "", opt, relocModel);

    module.setDataLayout(targetMachine->createDataLayout());

    // 验证模块 / Verify module
    if (llvm::verifyModule(module, &llvm::errs())) {
        std::cerr << "sukic: module verification failed\n";
        return false;
    }

    // 打开输出文件 / Open output file
    std::error_code ec;
    llvm::raw_fd_ostream dest(outputPath, ec, llvm::sys::fs::OF_None);
    if (ec) {
        std::cerr << "sukic: could not open file: " << ec.message() << "\n";
        return false;
    }

    // 生成目标文件 / Generate object file
    llvm::legacy::PassManager pass;

    // 添加优化 passes / Add optimization passes
    // 标准优化级别：mem2reg, instcombine, reassociate, GVN, simplifycfg
    // Standard optimization level: mem2reg, instcombine, reassociate, GVN, simplifycfg
    pass.add(llvm::createPromoteMemoryToRegisterPass());
    pass.add(llvm::createInstructionCombiningPass());
    pass.add(llvm::createReassociatePass());
    pass.add(llvm::createGVNPass());
    pass.add(llvm::createCFGSimplificationPass());

    auto fileType = llvm::CodeGenFileType::ObjectFile;

    if (targetMachine->addPassesToEmitFile(pass, dest, nullptr, fileType)) {
        std::cerr << "sukic: target machine cannot emit object file\n";
        return false;
    }

    pass.run(module);
    dest.flush();

    return true;
}

bool ObjectEmitter::emitAssembly(llvm::Module& module, const std::string& outputPath,
                                  const std::string& targetTriple) {
    std::string tripleStr = targetTriple.empty() ? getDefaultTargetTriple() : targetTriple;
    llvm::Triple triple(tripleStr);
    module.setTargetTriple(triple);

    std::string error;
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(tripleStr, error);
    if (!target) {
        std::cerr << "sukic: target not found: " << error << "\n";
        return false;
    }

    llvm::TargetOptions opt;
    auto relocModel = std::optional<llvm::Reloc::Model>();
    llvm::TargetMachine* targetMachine = target->createTargetMachine(
        triple, "generic", "", opt, relocModel);

    module.setDataLayout(targetMachine->createDataLayout());

    std::error_code ec;
    llvm::raw_fd_ostream dest(outputPath, ec, llvm::sys::fs::OF_None);
    if (ec) {
        std::cerr << "sukic: could not open file: " << ec.message() << "\n";
        return false;
    }

    llvm::legacy::PassManager pass;
    auto fileType = llvm::CodeGenFileType::AssemblyFile;

    if (targetMachine->addPassesToEmitFile(pass, dest, nullptr, fileType)) {
        std::cerr << "sukic: target machine cannot emit assembly\n";
        return false;
    }

    pass.run(module);
    dest.flush();

    return true;
}

bool ObjectEmitter::emitIR(llvm::Module& module, const std::string& outputPath) {
    std::error_code ec;
    llvm::raw_fd_ostream dest(outputPath, ec, llvm::sys::fs::OF_None);
    if (ec) {
        std::cerr << "sukic: could not open file: " << ec.message() << "\n";
        return false;
    }

    module.print(dest, nullptr);
    dest.flush();
    return true;
}

bool ObjectEmitter::linkExecutable(const std::vector<std::string>& objectFiles,
                                    const std::string& outputPath,
                                    const std::string& targetTriple) {
    if (objectFiles.empty()) {
        std::cerr << "sukic: no object files to link\n";
        return false;
    }

    // 构建链接命令 / Build link command
    std::string cmd;

#ifdef _WIN32
    // Windows: 使用 cl.exe 链接（MSVC 工具链）
    // cl.exe 可以链接 .obj 文件生成 .exe
    cmd = "cl.exe /nologo /Fe:\"" + outputPath + "\" /EHsc /utf-8";
    for (const auto& obj : objectFiles) {
        cmd += " \"" + obj + "\"";
    }
    // 添加 C 运行时库和 printf 支持
    cmd += " /link /SUBSYSTEM:CONSOLE /NOLOGO";
    cmd += " libcmt.lib legacy_stdio_definitions.lib";
#else
    // Unix: 使用 cc 链接
    cmd = "cc -o \"" + outputPath + "\"";
    for (const auto& obj : objectFiles) {
        cmd += " \"" + obj + "\"";
    }
    cmd += " -lc -lm";
#endif

    std::cerr << "sukic: linking: " << cmd << "\n";
    int result = std::system(cmd.c_str());

    return result == 0;
}

#endif

} // namespace suki
