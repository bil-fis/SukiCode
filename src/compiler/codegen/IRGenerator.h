#pragma once
// LLVM IR code generator for SukiCode.
// Translates the typed AST into LLVM IR.

#ifdef SUKI_HAS_LLVM
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Value.h>
#endif

#include "compiler/ast/ASTNode.h"
#include "compiler/diag/Diagnostic.h"
#include <memory>
#include <string>

namespace suki {

class IRGenerator {
public:
    IRGenerator(DiagnosticEngine& diag, const std::string& moduleName);
    ~IRGenerator();

    // Generate LLVM IR from a compilation unit
    // Returns true on success
    bool generate(const CompilationUnit& cu);

    // Get the generated LLVM IR as a string
    std::string getIRString() const;

    // Get the generated LLVM module (for linking)
#ifdef SUKI_HAS_LLVM
    std::unique_ptr<llvm::Module> releaseModule();
#endif

private:
    DiagnosticEngine& diag_;
    std::string moduleName_;

#ifdef SUKI_HAS_LLVM
    std::unique_ptr<llvm::LLVMContext> context_;
    std::unique_ptr<llvm::Module> module_;
    std::unique_ptr<llvm::IRBuilder<>> builder_;
#endif
};

} // namespace suki
