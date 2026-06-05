#pragma once
// Type checker for SukiCode semantic analysis.

#include "compiler/ast/ASTNode.h"
#include "compiler/diag/Diagnostic.h"

namespace suki {

class TypeChecker {
public:
    TypeChecker(DiagnosticEngine& diag);
    ~TypeChecker();

    // TODO: type checking methods

private:
    DiagnosticEngine& diag_;
};

} // namespace suki
