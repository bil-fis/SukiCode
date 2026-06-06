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
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/DebugInfoMetadata.h>
#endif

#include "compiler/ast/ASTNode.h"
#include "compiler/diag/Diagnostic.h"
#include "TypeConverter.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace suki {

class IRGenerator {
public:
    IRGenerator(DiagnosticEngine& diag, const std::string& moduleName);
    ~IRGenerator();

    // 启用调试信息生成 / Enable debug info generation
    void setEmitDebugInfo(bool emit) { emitDebugInfo_ = emit; }

    bool generate(const CompilationUnit& cu);
    std::string getIRString() const;

#ifdef SUKI_HAS_LLVM
    std::unique_ptr<llvm::Module> releaseModule();

private:
    // ─── 类型映射 / Type mapping ────────────────────────────────────────
    llvm::Type* resolveType(const TypeRepr* tr);
    llvm::Type* getLLVMType(const std::string& name);

    // ─── 声明代码生成 / Declaration codegen ────────────────────────────
    void genDecl(const Decl& decl);
    llvm::Function* genFunctionDecl(const FunctionDecl& decl);
    void genVariableDecl(const VariableDecl& decl);

    // ─── 语句代码生成 / Statement codegen ─────────────────────────────
    void genStmt(const Stmt& stmt);
    void genReturnStmt(const ReturnStmt& stmt);
    void genExprStmt(const ExpressionStmt& stmt);
    void genCompoundStmt(const CompoundStmt& stmt);
    void genIfStmt(const IfDecl& decl);
    void genWhileStmt(const WhileDecl& decl);
    void genForInStmt(const ForInDecl& decl);
    void genSwitchStmt(const SwitchDecl& decl);
    void genDoCatchStmt(const DoCatchDecl& decl);
    void genThrowStmt(const ThrowDecl& decl);

    // ─── 表达式代码生成 / Expression codegen ──────────────────────────
    llvm::Value* genExpr(const Expr& expr);
    llvm::Value* genIntegerLiteral(const IntegerLiteralExpr& expr, llvm::Type* expectedType = nullptr);
    llvm::Value* genFloatLiteral(const FloatLiteralExpr& expr);
    llvm::Value* genStringLiteral(const StringLiteralExpr& expr);
    llvm::Value* genBoolLiteral(const BoolLiteralExpr& expr);
    llvm::Value* genNilLiteral();
    llvm::Value* genIdentifier(const IdentifierExpr& expr);
    llvm::Value* genBinaryExpr(const BinaryExpr& expr);
    llvm::Value* genCallExpr(const CallExpr& expr);
    llvm::Value* genMemberAccess(const MemberAccessExpr& expr);
    llvm::Value* genArrayLiteral(const ArrayLiteralExpr& expr);
    llvm::Value* genDictLiteral(const DictLiteralExpr& expr);
    llvm::Value* genTupleExpr(const TupleExpr& expr);
    llvm::Value* genUnaryExpr(const UnaryExpr& expr);
    llvm::Value* genIfExpr(const IfExpr& expr);
    llvm::Value* genInterpolatedString(const InterpolatedStringExpr& expr);

    // ─── ARC 支持 / ARC support ─────────────────────────────────────────
    void insertRetain(llvm::Value* obj);
    void insertRelease(llvm::Value* obj);
    bool isReferenceType(llvm::Type* type) const;

    // ─── 辅助 / Helpers ────────────────────────────────────────────────
    llvm::AllocaInst* createEntryBlockAlloca(llvm::Function* func,
                                              llvm::Type* type,
                                              const std::string& name);
    llvm::Value* createStringGlobal(const std::string& str);
    void error(SourceLocation loc, const std::string& msg);

    llvm::LLVMContext context_;
    std::unique_ptr<llvm::Module> module_;
    std::unique_ptr<llvm::IRBuilder<>> builder_;
    TypeConverter typeConverter_;

    // 调试信息 / Debug info
    std::unique_ptr<llvm::DIBuilder> diBuilder_;
    llvm::DICompileUnit* diCompileUnit_ = nullptr;
    llvm::DIFile* diFile_ = nullptr;
    bool emitDebugInfo_ = false;
    void initDebugInfo(const std::string& filename);
    void finalizeDebugInfo();

    llvm::Function* currentFunc_ = nullptr;

    // 变量存储 / Variable storage (name -> alloca)
    std::unordered_map<std::string, llvm::Value*> namedValues_;
    std::unordered_map<std::string, llvm::Type*> namedTypes_;

    // 函数注册 / Function registry
    std::unordered_map<std::string, llvm::Function*> functions_;

    // 泛型函数实例化 / Generic function instantiation
    std::unordered_map<std::string, const FunctionDecl*> genericFuncAsts_;
    const CompilationUnit* currentCu_ = nullptr;

    // 协程状态 / Coroutine state
    bool isInAsyncFunc_ = false;
    int awaitPointCount_ = 0;
    llvm::Value* coroutineState_ = nullptr;

    // 闭包计数器 / Closure counter
    int nextClosureId_ = 0;

    // 控制流块 / Control flow blocks (for break/continue)
    struct LoopInfo {
        llvm::BasicBlock* condBlock;
        llvm::BasicBlock* afterBlock;
    };
    std::vector<LoopInfo> loopStack_;
#endif

    DiagnosticEngine& diag_;
    std::string moduleName_;
};

} // namespace suki
