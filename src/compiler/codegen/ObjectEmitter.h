#pragma once
// LLVM 目标代码输出器
// Emits object files and links executables using LLVM MC layer and LLD.

#ifdef SUKI_HAS_LLVM
#include <llvm/IR/Module.h>
#include <llvm/Target/TargetMachine.h>
#include <memory>
#include <string>
#endif

namespace suki {

class ObjectEmitter {
public:
    ObjectEmitter();
    ~ObjectEmitter();

    // 初始化 LLVM 目标 / Initialize LLVM targets
    static void initializeTargets();

    // 输出目标文件 / Emit object file
    // Returns true on success
#ifdef SUKI_HAS_LLVM
    bool emitObjectFile(llvm::Module& module, const std::string& outputPath,
                        const std::string& targetTriple = "");

    // 输出汇编文件 / Emit assembly file
    bool emitAssembly(llvm::Module& module, const std::string& outputPath,
                      const std::string& targetTriple = "");

    // 输出 LLVM IR 文件 / Emit LLVM IR file
    bool emitIR(llvm::Module& module, const std::string& outputPath);

    // 链接可执行文件 / Link executable
    // Uses LLD if available, falls back to system linker
    static bool linkExecutable(const std::vector<std::string>& objectFiles,
                               const std::string& outputPath,
                               const std::string& targetTriple = "");

    // 获取默认目标三元组 / Get default target triple
    static std::string getDefaultTargetTriple();
#endif
};

} // namespace suki
