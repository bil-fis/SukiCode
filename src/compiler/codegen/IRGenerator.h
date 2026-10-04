#pragma once

// LLVM IR generation — strict PIMPL.
//
// This header must NEVER include an LLVM header. Every LLVM type
// (llvm::Module, llvm::IRBuilder, ...) lives exclusively in the implementation
// file behind `Impl`. Callers interact only through the coarse-grained,
// LLVM-free API below. This keeps LLVM out of the frontend's include graph
// and avoids the compile-time blow-up of pulling LLVM into every TU.

#include "compiler/ast/AST.h"
#include "compiler/codegen/TargetInfo.h"

#include <memory>
#include <string>

namespace suki {

class Sema;

class IRGenerator {
public:
    explicit IRGenerator(const TargetInfo& target);
    ~IRGenerator();

    IRGenerator(const IRGenerator&) = delete;
    IRGenerator& operator=(const IRGenerator&) = delete;

    // Emit textual LLVM IR for a translation unit. On failure returns false and
    // fills `errOut` with a human-readable reason.
    bool emitIR(const NodeList& decls, std::string& irOut, std::string& errOut);

    // Emit IR for a checked translation unit. The analyser is passed along so
    // generic functions can be monomorphised: each recorded instantiation is
    // re-checked immediately before it is lowered, which is what puts concrete
    // types on the (shared) AST for that one function.
    bool emitIR(const NodeList& decls, Sema& sema, std::string& irOut,
                std::string& errOut);

    // Emit a native object file for a checked translation unit. As with emitIR,
    // the analyser is threaded through for generic monomorphisation. When
    // `irOut` is non-null the textual IR is also returned (the driver uses it to
    // locate `@main`). On failure returns false and fills `errOut`.
    //
    // BLK-B: this lowers IR -> object entirely in-process via LLVM's
    // TargetMachine + MC/AsmPrinter backend. No external `clang -c` is invoked,
    // so the driver no longer depends on a C/IR frontend to produce an object.
    bool emitObject(const NodeList& decls, Sema& sema,
                    const std::string& objPath, std::string* irOut,
                    std::string& errOut);

    const TargetInfo& target() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace suki
