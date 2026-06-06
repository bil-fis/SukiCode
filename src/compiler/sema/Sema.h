#pragma once
// SukiCode 语义分析器
// Semantic analyzer for SukiCode. Performs type checking, scope resolution,
// and validation on the parsed AST.

#include "compiler/ast/ASTNode.h"
#include "compiler/diag/Diagnostic.h"
#include "SymbolTable.h"
#include "TypeChecker.h"
#include "Type.h"
#include <memory>
#include <unordered_set>

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
    TypeChecker typeChecker_;
    std::string currentModule_;
    TypePtr currentReturnType_; // Current function's return type for checking
    std::vector<std::string> importedModules_; // 已导入模块列表
    std::unordered_set<std::string> movedVariables_; // 已移动的变量集合
    std::unordered_set<std::string> actorTypes_; // Actor 类型名称集合
    std::string currentActor_; // 当前 Actor 名称（如果在 Actor 内部）
    std::string currentTypeName_; // 当前处理的类型名称（用于 self/super 引用）
};

} // namespace suki
