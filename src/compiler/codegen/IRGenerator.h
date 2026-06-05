#pragma once
// SukiCode LLVM IR 代码生成器
// Generates LLVM IR from the typed AST.

#ifdef SUKI_HAS_LLVM
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Value.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/BasicBlock.h>
#endif

#include "compiler/ast/ASTNode.h"
#include "compiler/diag/Diagnostic.h"
#include "TypeConverter.h"
#include "compiler/sema/SymbolTable.h"
#include <memory>
#include <string>
#include <unordered_map>

namespace suki {

class IRGenerator {
public:
    IRGenerator(DiagnosticEngine& diag, const std::string& moduleName);
    ~IRGenerator();

    // Generate LLVM IR from a compilation unit
    bool generate(const CompilationUnit& cu);

    // Get the generated LLVM IR as a string
    std::string getIRString() const;

    // Get the generated LLVM module
#ifdef SUKI_HAS_LLVM
    std::unique_ptr<llvm::Module> releaseModule();
#endif

private:
#ifdef SUKI_HAS_LLVM
    // ─── Declaration codegen ─────────────────────────────────────────────
    void genDecl(const Decl& decl);
    llvm::Function* genFunctionDecl(const FunctionDecl& decl);
    void genVariableDecl(const VariableDecl& decl);

    // ─── Statement codegen ──────────────────────────────────────────────
    void genStmt(const Stmt& stmt);
    void genReturnStmt(const ReturnStmt& stmt);
    void genExprStmt(const ExpressionStmt& stmt);
    void genIfDecl(const IfDecl& decl);
    void genWhileDecl(const WhileDecl& decl);
    void genForInDecl(const ForInDecl& decl);

    // ─── Expression codegen ─────────────────────────────────────────────
    llvm::Value* genExpr(const Expr& expr);
    llvm::Value* genIntegerLiteral(const IntegerLiteralExpr& expr);
    llvm::Value* genFloatLiteral(const FloatLiteralExpr& expr);
    llvm::Value* genStringLiteral(const StringLiteralExpr& expr);
    llvm::Value* genBoolLiteral(const BoolLiteralExpr& expr);
    llvm::Value* genIdentifier(const IdentifierExpr& expr);
    llvm::Value* genBinaryExpr(const BinaryExpr& expr);
    llvm::Value* genCallExpr(const CallExpr& expr);
    llvm::Value* genMemberAccess(const MemberAccessExpr& expr);

    // ─── Helpers ────────────────────────────────────────────────────────
    llvm::AllocaInst* createEntryBlockAlloca(llvm::Function* func,
                                              llvm::Type* type,
                                              const std::string& name);
    void error(SourceLocation loc, const std::string& msg);

    llvm::LLVMContext context_;
    std::unique_ptr<llvm::Module> module_;
    std::unique_ptr<llvm::IRBuilder<>> builder_;
    TypeConverter typeConverter_;

    // Current function being generated
    llvm::Function* currentFunc_ = nullptr;

    // Variable storage (name -> alloca)
    std::unordered_map<std::string, llvm::Value*> namedValues_;

    // Function registry
    std::unordered_map<std::string, llvm::Function*> functions_;
#endif

    DiagnosticEngine& diag_;
    std::string moduleName_;
};

} // namespace suki
