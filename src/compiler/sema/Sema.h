#pragma once
// SukiCode 语义分析器
// Semantic analyzer for SukiCode. Performs type checking, scope resolution,
// and validation on the parsed AST.

#include "compiler/ast/ASTNode.h"
#include "compiler/diag/Diagnostic.h"
#include "SymbolTable.h"
#include "Type.h"
#include <memory>

namespace suki {

class Sema {
public:
    Sema(DiagnosticEngine& diag);
    ~Sema();

    // 运行语义分析 / Run semantic analysis on a compilation unit
    bool analyze(CompilationUnit& cu);

private:
    // ─── 声明处理 / Declaration processing ───────────────────────────────
    void processDecl(Decl& decl);
    void processVariableDecl(VariableDecl& decl);
    void processFunctionDecl(FunctionDecl& decl);
    void processStructDecl(StructDecl& decl);
    void processClassDecl(ClassDecl& decl);
    void processEnumDecl(EnumDecl& decl);

    // ─── 语句处理 / Statement processing ─────────────────────────────────
    void processStmt(Stmt& stmt);
    void processReturnStmt(ReturnStmt& stmt);
    void processExprStmt(ExpressionStmt& stmt);

    // ─── 表达式类型推断 / Expression type inference ──────────────────────
    TypePtr inferExprType(Expr& expr);
    TypePtr inferIntegerLiteral(IntegerLiteralExpr& expr);
    TypePtr inferFloatLiteral(FloatLiteralExpr& expr);
    TypePtr inferStringLiteral(StringLiteralExpr& expr);
    TypePtr inferBoolLiteral(BoolLiteralExpr& expr);
    TypePtr inferIdentifier(IdentifierExpr& expr);
    TypePtr inferBinaryExpr(BinaryExpr& expr);
    TypePtr inferCallExpr(CallExpr& expr);
    TypePtr inferMemberAccess(MemberAccessExpr& expr);

    // ─── 类型解析 / Type resolution ──────────────────────────────────────
    TypePtr resolveTypeRepr(const TypeRepr& typeRepr);

    // ─── 辅助方法 / Helper methods ──────────────────────────────────────
    void error(SourceLocation loc, const std::string& message);
    void warning(SourceLocation loc, const std::string& message);

    DiagnosticEngine& diag_;
    SymbolTable symbols_;
    std::string currentModule_;
};

} // namespace suki
