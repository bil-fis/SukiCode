// SukiCode LLVM IR code generator implementation.
// Generates LLVM IR from the typed AST.

#include "IRGenerator.h"

#ifdef SUKI_HAS_LLVM
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>
#endif

namespace suki {

IRGenerator::IRGenerator(DiagnosticEngine& diag, const std::string& moduleName)
    : diag_(diag), moduleName_(moduleName)
#ifdef SUKI_HAS_LLVM
      , context_(), typeConverter_(context_)
#endif
{
#ifdef SUKI_HAS_LLVM
    module_ = std::make_unique<llvm::Module>(moduleName, context_);
    builder_ = std::make_unique<llvm::IRBuilder<>>(context_);
#endif
}

IRGenerator::~IRGenerator() = default;

bool IRGenerator::generate(const CompilationUnit& cu) {
#ifdef SUKI_HAS_LLVM
    // Generate IR for all declarations
    for (const auto& decl : cu.declarations) {
        if (decl) genDecl(*decl);
    }

    // If no main function was generated, create a default one
    if (!module_->getFunction("main")) {
        llvm::FunctionType* mainType = llvm::FunctionType::get(
            llvm::Type::getInt32Ty(context_), false);
        llvm::Function* mainFunc = llvm::Function::Create(
            mainType, llvm::Function::ExternalLinkage, "main", module_.get());

        llvm::BasicBlock* entry = llvm::BasicBlock::Create(context_, "entry", mainFunc);
        builder_->SetInsertPoint(entry);
        builder_->CreateRet(llvm::ConstantInt::get(
            llvm::Type::getInt32Ty(context_), 0));
    }

    // Verify the module
    std::string error;
    llvm::raw_string_ostream errorStream(error);
    if (llvm::verifyModule(*module_, &errorStream)) {
        diag_.error({}, moduleName_, "LLVM module verification failed: " + error);
        return false;
    }

    return !diag_.hadErrors();
#else
    diag_.error({}, moduleName_, "LLVM not available");
    return false;
#endif
}

std::string IRGenerator::getIRString() const {
#ifdef SUKI_HAS_LLVM
    std::string ir;
    llvm::raw_string_ostream os(ir);
    module_->print(os, nullptr);
    return ir;
#else
    return "; LLVM not available\n";
#endif
}

#ifdef SUKI_HAS_LLVM
std::unique_ptr<llvm::Module> IRGenerator::releaseModule() {
    return std::move(module_);
}

// ─── Declaration codegen ─────────────────────────────────────────────────

void IRGenerator::genDecl(const Decl& decl) {
    switch (decl.declKind) {
        case DeclKind::Function:
            genFunctionDecl(static_cast<const FunctionDecl&>(decl));
            break;
        case DeclKind::Variable:
            genVariableDecl(static_cast<const VariableDecl&>(decl));
            break;
        default:
            // Other declarations not yet implemented
            break;
    }
}

llvm::Function* IRGenerator::genFunctionDecl(const FunctionDecl& decl) {
    // Check if function already exists
    auto it = functions_.find(decl.name);
    if (it != functions_.end()) return it->second;

    // Create function type
    std::vector<llvm::Type*> paramTypes;
    for (const auto& param : decl.params) {
        if (param.type) {
            // For now, use Int64 for all parameters as placeholder
            paramTypes.push_back(llvm::Type::getInt64Ty(context_));
        }
    }

    llvm::Type* returnType = decl.returnType ?
        llvm::Type::getInt64Ty(context_) : // placeholder
        llvm::Type::getVoidTy(context_);

    llvm::FunctionType* funcType = llvm::FunctionType::get(returnType, paramTypes, false);

    // Create function
    llvm::Function* func = llvm::Function::Create(
        funcType, llvm::Function::ExternalLinkage, decl.name, module_.get());

    // Set parameter names
    size_t idx = 0;
    for (auto& arg : func->args()) {
        if (idx < decl.params.size()) {
            arg.setName(decl.params[idx].internalName);
        }
        idx++;
    }

    // Register function
    functions_[decl.name] = func;

    // Generate body if present
    if (!decl.body.empty()) {
        llvm::BasicBlock* entry = llvm::BasicBlock::Create(context_, "entry", func);
        builder_->SetInsertPoint(entry);

        // Save current function
        llvm::Function* prevFunc = currentFunc_;
        currentFunc_ = func;

        // Clear named values for new scope
        namedValues_.clear();

        // Create allocas for parameters
        idx = 0;
        for (auto& arg : func->args()) {
            llvm::AllocaInst* alloca = createEntryBlockAlloca(
                func, arg.getType(), std::string(arg.getName()));
            builder_->CreateStore(&arg, alloca);
            namedValues_[std::string(arg.getName())] = alloca;
            idx++;
        }

        // Generate body
        for (const auto& stmt : decl.body) {
            if (stmt) genStmt(*stmt);
        }

        // Add return if missing
        if (!builder_->GetInsertBlock()->getTerminator()) {
            if (returnType->isVoidTy()) {
                builder_->CreateRetVoid();
            } else {
                builder_->CreateRet(llvm::ConstantInt::get(
                    llvm::Type::getInt64Ty(context_), 0));
            }
        }

        // Verify function
        llvm::verifyFunction(*func);

        // Restore previous function
        currentFunc_ = prevFunc;
    }

    return func;
}

void IRGenerator::genVariableDecl(const VariableDecl& decl) {
    if (!currentFunc_) return;

    // Get variable name
    std::string name;
    if (decl.pattern && decl.pattern->patternKind == PatternKind::Identifier) {
        name = static_cast<const IdentifierPattern*>(decl.pattern.get())->name;
    } else {
        return;
    }

    // Create alloca
    llvm::Type* type = llvm::Type::getInt64Ty(context_); // placeholder
    llvm::AllocaInst* alloca = createEntryBlockAlloca(currentFunc_, type, name);

    // Store initializer if present
    if (decl.initializer) {
        llvm::Value* initVal = genExpr(*decl.initializer);
        if (initVal) {
            builder_->CreateStore(initVal, alloca);
        }
    }

    namedValues_[name] = alloca;
}

// ─── Statement codegen ───────────────────────────────────────────────────

void IRGenerator::genStmt(const Stmt& stmt) {
    switch (stmt.stmtKind) {
        case StmtKind::Return:
            genReturnStmt(static_cast<const ReturnStmt&>(stmt));
            break;
        case StmtKind::Expression:
            genExprStmt(static_cast<const ExpressionStmt&>(stmt));
            break;
        case StmtKind::VariableDecl: {
            auto& varStmt = static_cast<const VariableDeclStmt&>(stmt);
            if (varStmt.varDecl) {
                genVariableDecl(static_cast<const VariableDecl&>(*varStmt.varDecl));
            }
            break;
        }
        case StmtKind::DeclStmt: {
            auto& declStmt = static_cast<const DeclStmt&>(stmt);
            if (declStmt.decl) genDecl(*declStmt.decl);
            break;
        }
        default:
            break;
    }
}

void IRGenerator::genReturnStmt(const ReturnStmt& stmt) {
    if (stmt.value) {
        llvm::Value* retVal = genExpr(*stmt.value);
        if (retVal) {
            builder_->CreateRet(retVal);
        }
    } else {
        builder_->CreateRetVoid();
    }
}

void IRGenerator::genExprStmt(const ExpressionStmt& stmt) {
    if (stmt.expression) {
        genExpr(*stmt.expression);
    }
}

// ─── Expression codegen ──────────────────────────────────────────────────

llvm::Value* IRGenerator::genExpr(const Expr& expr) {
    switch (expr.exprKind) {
        case ExprKind::IntegerLiteral:
            return genIntegerLiteral(static_cast<const IntegerLiteralExpr&>(expr));
        case ExprKind::FloatLiteral:
            return genFloatLiteral(static_cast<const FloatLiteralExpr&>(expr));
        case ExprKind::StringLiteral:
            return genStringLiteral(static_cast<const StringLiteralExpr&>(expr));
        case ExprKind::BoolLiteral:
            return genBoolLiteral(static_cast<const BoolLiteralExpr&>(expr));
        case ExprKind::Identifier:
            return genIdentifier(static_cast<const IdentifierExpr&>(expr));
        case ExprKind::Binary:
            return genBinaryExpr(static_cast<const BinaryExpr&>(expr));
        case ExprKind::Call:
            return genCallExpr(static_cast<const CallExpr&>(expr));
        case ExprKind::MemberAccess:
            return genMemberAccess(static_cast<const MemberAccessExpr&>(expr));
        default:
            return nullptr;
    }
}

llvm::Value* IRGenerator::genIntegerLiteral(const IntegerLiteralExpr& expr) {
    return llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), expr.value, true);
}

llvm::Value* IRGenerator::genFloatLiteral(const FloatLiteralExpr& expr) {
    return llvm::ConstantFP::get(context_, llvm::APFloat(expr.value));
}

llvm::Value* IRGenerator::genStringLiteral(const StringLiteralExpr& expr) {
    // Create global string constant
    llvm::Constant* strConst = llvm::ConstantDataArray::getString(context_, expr.value);
    llvm::GlobalVariable* global = new llvm::GlobalVariable(
        *module_, strConst->getType(), true,
        llvm::GlobalValue::PrivateLinkage, strConst, ".str");

    // Return pointer to string data
    return builder_->CreatePointerCast(global, llvm::PointerType::get(context_, 0));
}

llvm::Value* IRGenerator::genBoolLiteral(const BoolLiteralExpr& expr) {
    return llvm::ConstantInt::get(llvm::Type::getInt1Ty(context_), expr.value ? 1 : 0);
}

llvm::Value* IRGenerator::genIdentifier(const IdentifierExpr& expr) {
    auto it = namedValues_.find(expr.name);
    if (it != namedValues_.end()) {
        return builder_->CreateLoad(llvm::Type::getInt64Ty(context_),
                                    it->second, expr.name);
    }

    // Check if it's a function
    auto funcIt = functions_.find(expr.name);
    if (funcIt != functions_.end()) {
        return funcIt->second;
    }

    error({}, "unknown identifier: " + expr.name);
    return nullptr;
}

llvm::Value* IRGenerator::genBinaryExpr(const BinaryExpr& expr) {
    llvm::Value* left = genExpr(*expr.left);
    llvm::Value* right = genExpr(*expr.right);
    if (!left || !right) return nullptr;

    // Ensure both are the same type (promote if needed)
    if (left->getType() != right->getType()) {
        // Simple promotion: extend smaller integer
        if (left->getType()->isIntegerTy() && right->getType()->isIntegerTy()) {
            if (left->getType()->getIntegerBitWidth() < right->getType()->getIntegerBitWidth()) {
                left = builder_->CreateSExt(left, right->getType());
            } else if (right->getType()->getIntegerBitWidth() < left->getType()->getIntegerBitWidth()) {
                right = builder_->CreateSExt(right, left->getType());
            }
        }
    }

    switch (expr.op) {
        case TokenKind::Plus:   return builder_->CreateAdd(left, right, "addtmp");
        case TokenKind::Minus:  return builder_->CreateSub(left, right, "subtmp");
        case TokenKind::Star:   return builder_->CreateMul(left, right, "multmp");
        case TokenKind::Slash:  return builder_->CreateSDiv(left, right, "divtmp");
        case TokenKind::Percent: return builder_->CreateSRem(left, right, "modtmp");
        case TokenKind::Equal:  return builder_->CreateICmpEQ(left, right, "eqtmp");
        case TokenKind::NotEqual: return builder_->CreateICmpNE(left, right, "netmp");
        case TokenKind::Less:   return builder_->CreateICmpSLT(left, right, "slttmp");
        case TokenKind::Greater: return builder_->CreateICmpSGT(left, right, "sgttmp");
        case TokenKind::LessEqual: return builder_->CreateICmpSLE(left, right, "sletmp");
        case TokenKind::GreaterEqual: return builder_->CreateICmpSGE(left, right, "sgetmp");
        case TokenKind::AmpAmp: return builder_->CreateLogicalAnd(left, right, "andtmp");
        case TokenKind::PipePipe: return builder_->CreateLogicalOr(left, right, "ortmp");
        case TokenKind::Amp:    return builder_->CreateAnd(left, right, "bitand");
        case TokenKind::Pipe:   return builder_->CreateOr(left, right, "bitor");
        case TokenKind::Caret:  return builder_->CreateXor(left, right, "bitxor");
        case TokenKind::LShift: return builder_->CreateShl(left, right, "shltmp");
        case TokenKind::RShift: return builder_->CreateAShr(left, right, "ashrtmp");
        default:
            error({}, "unsupported binary operator");
            return nullptr;
    }
}

llvm::Value* IRGenerator::genCallExpr(const CallExpr& expr) {
    if (expr.callee->exprKind != ExprKind::Identifier) {
        error({}, "only direct function calls are supported");
        return nullptr;
    }

    auto& funcName = static_cast<const IdentifierExpr&>(*expr.callee).name;

    // Handle built-in print function
    if (funcName == "print") {
        // For now, just generate a placeholder
        if (!expr.args.empty()) {
            return genExpr(*expr.args[0].value);
        }
        return nullptr;
    }

    llvm::Function* func = module_->getFunction(funcName);
    if (!func) {
        error({}, "unknown function: " + funcName);
        return nullptr;
    }

    // Generate arguments
    std::vector<llvm::Value*> args;
    for (const auto& arg : expr.args) {
        llvm::Value* argVal = genExpr(*arg.value);
        if (!argVal) return nullptr;
        args.push_back(argVal);
    }

    // Check argument count
    if (args.size() != func->arg_size()) {
        error({}, "wrong number of arguments for " + funcName);
        return nullptr;
    }

    return builder_->CreateCall(func, args, "calltmp");
}

llvm::Value* IRGenerator::genMemberAccess(const MemberAccessExpr& expr) {
    // TODO: implement member access codegen
    return nullptr;
}

// ─── Helpers ─────────────────────────────────────────────────────────────

llvm::AllocaInst* IRGenerator::createEntryBlockAlloca(llvm::Function* func,
                                                       llvm::Type* type,
                                                       const std::string& name) {
    llvm::IRBuilder<> tmpB(&func->getEntryBlock(), func->getEntryBlock().begin());
    return tmpB.CreateAlloca(type, nullptr, name);
}

void IRGenerator::error(SourceLocation loc, const std::string& msg) {
    diag_.error(loc, moduleName_, msg);
}

#endif

} // namespace suki
