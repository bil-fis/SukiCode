#pragma once
// Semantic analysis for SukiCode.
// Performs type checking, scope resolution, and validation.

#include "compiler/ast/ASTNode.h"
#include "compiler/diag/Diagnostic.h"

namespace suki {

class Sema {
public:
    Sema(DiagnosticEngine& diag);
    ~Sema();

    // Run semantic analysis on a compilation unit
    bool analyze(CompilationUnit& cu);

private:
    DiagnosticEngine& diag_;
};

} // namespace suki
