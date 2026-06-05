#pragma once
// AST pretty-printer for debugging and -emit-ast output.

#include "ASTNode.h"
#include <string>
#include <sstream>

namespace suki {

class ASTPrinter {
public:
    std::string print(const CompilationUnit& cu);

private:
    void printDecl(const Decl& decl, int indent);
    void printStmt(const Stmt& stmt, int indent);
    void printExpr(const Expr& expr, int indent);
    void printTypeRepr(const TypeRepr& type, int indent);
    void printPattern(const Pattern& pattern, int indent);

    void indent(int level);
    std::ostringstream out_;
};

} // namespace suki
