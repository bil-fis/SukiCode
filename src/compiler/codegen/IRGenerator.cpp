// SukiCode LLVM IR code generator implementation.
// Generates LLVM IR from the typed AST.

#include "IRGenerator.h"

#ifdef SUKI_HAS_LLVM
#include <llvm/Support/TargetSelect.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/InlineAsm.h>
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

#ifdef SUKI_HAS_LLVM
void IRGenerator::initDebugInfo(const std::string& filename) {
    if (!emitDebugInfo_) return;

    diBuilder_ = std::make_unique<llvm::DIBuilder>(*module_);

    // 创建编译单元 / Create compile unit
    diFile_ = diBuilder_->createFile(filename, ".");
    diCompileUnit_ = diBuilder_->createCompileUnit(
        llvm::dwarf::DW_LANG_C_plus_plus, // 使用 C++ 语言标识
        diFile_,
        "SukiCode Compiler",
        false, // isOptimized
        "",    // flags
        0      // runtime version
    );

    // 设置模块调试标志
    module_->addModuleFlag(llvm::Module::Warning, "Debug Info Version",
                           llvm::DEBUG_METADATA_VERSION);
    module_->addModuleFlag(llvm::Module::Warning, "Dwarf Version", 4);
}

void IRGenerator::finalizeDebugInfo() {
    if (!emitDebugInfo_ || !diBuilder_) return;
    diBuilder_->finalize();
}
#endif

bool IRGenerator::generate(const CompilationUnit& cu) {
#ifdef SUKI_HAS_LLVM
    // 初始化调试信息 / Initialize debug info
    initDebugInfo(cu.filename);

    // 保存 CU 指针 / Save CU pointer
    currentCu_ = &cu;

    // 预注册所有函数声明 / Pre-register all function declarations
    for (const auto& decl : cu.declarations) {
        if (decl && decl->declKind == DeclKind::Function) {
            auto& fd = static_cast<const FunctionDecl&>(*decl);

            // 存储泛型函数 AST / Store generic function AST
            if (!fd.genericParams.empty()) {
                genericFuncAsts_[fd.name] = &fd;
                // 泛型函数在预注册时使用 i64 作为占位符
                // Generic functions use i64 as placeholder during pre-registration
            }

            std::vector<llvm::Type*> paramTypes;
            for (const auto& param : fd.params) {
                paramTypes.push_back(resolveType(param.type.get()));
            }
            llvm::Type* retType = fd.returnType ?
                resolveType(fd.returnType.get()) :
                llvm::Type::getVoidTy(context_);
            llvm::FunctionType* funcType = llvm::FunctionType::get(retType, paramTypes, false);
            llvm::Function* func = llvm::Function::Create(
                funcType, llvm::Function::ExternalLinkage, fd.name, module_.get());
            // 设置参数名称 / Set parameter names
            size_t argIdx = 0;
            for (auto& arg : func->args()) {
                if (argIdx < fd.params.size()) {
                    arg.setName(fd.params[argIdx].internalName);
                }
                argIdx++;
            }
            functions_[fd.name] = func;
        }
    }

    // 生成所有声明 / Generate all declarations
    for (const auto& decl : cu.declarations) {
        if (decl) genDecl(*decl);
    }

    // 如果没有 main 函数，创建默认的 / Create default main if missing
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

    // 验证模块 / Verify module
    std::string err;
    llvm::raw_string_ostream errOS(err);
    if (llvm::verifyModule(*module_, &errOS)) {
        diag_.error({}, moduleName_, "LLVM module verification failed: " + err);
        return false;
    }

    // 完成调试信息 / Finalize debug info
    finalizeDebugInfo();

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

// ─── 类型映射 / Type mapping ────────────────────────────────────────────

llvm::Type* IRGenerator::resolveType(const TypeRepr* tr) {
    if (!tr) return llvm::Type::getInt64Ty(context_); // 默认 Int

    if (tr->typeReprKind == TypeReprKind::Named) {
        auto& n = static_cast<const NamedTypeRepr&>(*tr);
        return getLLVMType(n.name);
    }
    if (tr->typeReprKind == TypeReprKind::Array) {
        // Array: { i8*, i64, i64 }
        return llvm::StructType::getTypeByName(context_, "suki.Array") ?
            llvm::StructType::getTypeByName(context_, "suki.Array") :
            llvm::StructType::create(context_, "suki.Array");
    }
    if (tr->typeReprKind == TypeReprKind::Dictionary) {
        return llvm::PointerType::get(context_, 0);
    }
    if (tr->typeReprKind == TypeReprKind::Optional) {
        auto& o = static_cast<const OptionalTypeRepr&>(*tr);
        return resolveType(o.base.get()); // 简化：Optional 和 base 类型相同
    }
    if (tr->typeReprKind == TypeReprKind::Tuple) {
        auto& t = static_cast<const TupleTypeRepr&>(*tr);
        std::vector<llvm::Type*> elems;
        for (const auto& e : t.elements) elems.push_back(resolveType(e.type.get()));
        return llvm::StructType::get(context_, elems);
    }
    if (tr->typeReprKind == TypeReprKind::Function) {
        return llvm::PointerType::get(context_, 0);
    }
    return llvm::Type::getInt64Ty(context_);
}

llvm::Type* IRGenerator::getLLVMType(const std::string& name) {
    if (name == "Void" || name == "void")   return llvm::Type::getVoidTy(context_);
    if (name == "Bool" || name == "bool")   return llvm::Type::getInt1Ty(context_);
    if (name == "Int8")                      return llvm::Type::getInt8Ty(context_);
    if (name == "Int16")                     return llvm::Type::getInt16Ty(context_);
    if (name == "Int32")                     return llvm::Type::getInt32Ty(context_);
    if (name == "Int" || name == "Int64")    return llvm::Type::getInt64Ty(context_);
    if (name == "UInt8")                     return llvm::Type::getInt8Ty(context_);
    if (name == "UInt16")                    return llvm::Type::getInt16Ty(context_);
    if (name == "UInt32")                    return llvm::Type::getInt32Ty(context_);
    if (name == "UInt" || name == "UInt64")  return llvm::Type::getInt64Ty(context_);
    if (name == "Float")                     return llvm::Type::getFloatTy(context_);
    if (name == "Double")                    return llvm::Type::getDoubleTy(context_);
    if (name == "Char")                      return llvm::Type::getInt32Ty(context_);
    if (name == "String") {
        // String: { i8*, i64 }
        return llvm::StructType::get(context_, {
            llvm::PointerType::get(context_, 0),
            llvm::Type::getInt64Ty(context_)
        });
    }
    // 未知类型默认为 i64
    return llvm::Type::getInt64Ty(context_);
}

// ─── 声明代码生成 / Declaration codegen ─────────────────────────────────

void IRGenerator::genDecl(const Decl& decl) {
    switch (decl.declKind) {
        case DeclKind::Function:
            genFunctionDecl(static_cast<const FunctionDecl&>(decl));
            break;
        case DeclKind::Variable:
            genVariableDecl(static_cast<const VariableDecl&>(decl));
            break;
        case DeclKind::If:
            genIfStmt(static_cast<const IfDecl&>(decl));
            break;
        case DeclKind::While:
            genWhileStmt(static_cast<const WhileDecl&>(decl));
            break;
        case DeclKind::ForIn:
            genForInStmt(static_cast<const ForInDecl&>(decl));
            break;
        case DeclKind::Switch:
            genSwitchStmt(static_cast<const SwitchDecl&>(decl));
            break;
        case DeclKind::DoCatch:
            genDoCatchStmt(static_cast<const DoCatchDecl&>(decl));
            break;
        case DeclKind::Throw:
            genThrowStmt(static_cast<const ThrowDecl&>(decl));
            break;
        case DeclKind::Asm: {
            auto& asmDecl = static_cast<const AsmDecl&>(decl);
            // 生成 LLVM 内联汇编 / Generate LLVM inline assembly
            llvm::InlineAsm* inlineAsm = llvm::InlineAsm::get(
                llvm::FunctionType::get(llvm::Type::getVoidTy(context_), false),
                asmDecl.assembly,
                asmDecl.constraints,
                asmDecl.hasSideEffects
            );
            builder_->CreateCall(inlineAsm);
            break;
        }
        default:
            break;
    }
}

llvm::Function* IRGenerator::genFunctionDecl(const FunctionDecl& decl) {
    llvm::Function* func = module_->getFunction(decl.name);
    if (!func) {
        error(decl.loc, "function not pre-registered: " + decl.name);
        return nullptr;
    }

    // 处理属性 / Process attributes
    for (const auto& attr : decl.attributes) {
        if (attr.name == "_cdecl") {
            func->setCallingConv(llvm::CallingConv::C);
        }
        if (attr.name == "no_mangle") {
            // 已经使用函数名作为链接名，无需额外处理
        }
    }

    // 如果没有函数体，只是声明 / If no body, just a declaration
    if (decl.body.empty()) return func;

    // 创建入口基本块 / Create entry basic block
    llvm::BasicBlock* entry = llvm::BasicBlock::Create(context_, "entry", func);
    builder_->SetInsertPoint(entry);

    llvm::Function* prevFunc = currentFunc_;
    bool prevAsync = isInAsyncFunc_;
    currentFunc_ = func;
    isInAsyncFunc_ = decl.isAsync;
    awaitPointCount_ = 0;
    namedValues_.clear();
    namedTypes_.clear();

    // 为参数创建 alloca / Create allocas for parameters
    size_t idx = 0;
    for (auto& arg : func->args()) {
        llvm::Type* paramType = resolveType(decl.params[idx].type.get());
        llvm::AllocaInst* allocaInst = createEntryBlockAlloca(func, paramType, std::string(arg.getName()));
        builder_->CreateStore(&arg, allocaInst);
        namedValues_[std::string(arg.getName())] = allocaInst;
        namedTypes_[std::string(arg.getName())] = paramType;
        idx++;
    }

    // 生成函数体 / Generate function body
    for (const auto& stmt : decl.body) {
        if (stmt) genStmt(*stmt);
    }

    // 如果没有终结指令，添加 return / Add return if missing
    if (!builder_->GetInsertBlock()->getTerminator()) {
        llvm::Type* retType = func->getReturnType();
        if (retType->isVoidTy()) {
            builder_->CreateRetVoid();
        } else {
            builder_->CreateRet(llvm::Constant::getNullValue(retType));
        }
    }

    llvm::verifyFunction(*func);
    currentFunc_ = prevFunc;
    isInAsyncFunc_ = prevAsync;
    return func;
}

void IRGenerator::genVariableDecl(const VariableDecl& decl) {
    if (!currentFunc_) return;

    // 获取变量名
    if (!decl.pattern || decl.pattern->patternKind != PatternKind::Identifier) return;
    std::string varName = static_cast<const IdentifierPattern*>(decl.pattern.get())->name;

    // 获取类型
    llvm::Type* varType = resolveType(decl.typeAnnotation.get());

    // 生成初始化值
    llvm::Value* initVal = nullptr;
    if (decl.initializer) {
        initVal = genExpr(*decl.initializer);
        if (initVal) varType = initVal->getType();
    }

    // 创建 alloca
    llvm::AllocaInst* allocaInst = createEntryBlockAlloca(currentFunc_, varType, varName);

    // 存储初始值
    if (initVal) {
        builder_->CreateStore(initVal, allocaInst);
        // ARC: 引用类型赋值时 retain / ARC: retain on reference type assignment
        if (isReferenceType(initVal->getType())) {
            insertRetain(initVal);
        }
    }

    // 注册变量
    namedValues_[varName] = allocaInst;
    namedTypes_[varName] = varType;
}

// ─── 语句代码生成 / Statement codegen ──────────────────────────────────

void IRGenerator::genStmt(const Stmt& stmt) {
    switch (stmt.stmtKind) {
        case StmtKind::Return:     genReturnStmt(static_cast<const ReturnStmt&>(stmt)); break;
        case StmtKind::Expression: genExprStmt(static_cast<const ExpressionStmt&>(stmt)); break;
        case StmtKind::Compound:   genCompoundStmt(static_cast<const CompoundStmt&>(stmt)); break;
        case StmtKind::VariableDecl: {
            auto& vs = static_cast<const VariableDeclStmt&>(stmt);
            if (vs.varDecl) genVariableDecl(static_cast<const VariableDecl&>(*vs.varDecl));
            break;
        }
        case StmtKind::DeclStmt: {
            auto& ds = static_cast<const DeclStmt&>(stmt);
            if (ds.decl) genDecl(*ds.decl);
            break;
        }
        case StmtKind::Break: {
            if (!loopStack_.empty()) {
                builder_->CreateBr(loopStack_.back().afterBlock);
            }
            break;
        }
        case StmtKind::Continue: {
            if (!loopStack_.empty()) {
                builder_->CreateBr(loopStack_.back().condBlock);
            }
            break;
        }
        default: break;
    }
}

void IRGenerator::genReturnStmt(const ReturnStmt& stmt) {
    // ARC: release all local reference type variables before return
    for (const auto& [name, allocaInst] : namedValues_) {
        auto typeIt = namedTypes_.find(name);
        if (typeIt != namedTypes_.end() && isReferenceType(typeIt->second)) {
            llvm::Value* val = builder_->CreateLoad(typeIt->second, allocaInst, "arc.load");
            insertRelease(val);
        }
    }

    if (stmt.value) {
        llvm::Value* retVal = genExpr(*stmt.value);
        if (retVal) {
            // 类型转换 / Type conversion
            llvm::Type* expectedType = currentFunc_->getReturnType();
            if (retVal->getType() != expectedType) {
                if (expectedType->isDoubleTy() && retVal->getType()->isIntegerTy()) {
                    retVal = builder_->CreateSIToFP(retVal, expectedType, "retcast");
                } else if (expectedType->isIntegerTy(64) && retVal->getType()->isIntegerTy()) {
                    retVal = builder_->CreateSExt(retVal, expectedType, "retcast");
                } else if (expectedType->isPointerTy() && retVal->getType()->isIntegerTy()) {
                    retVal = builder_->CreateIntToPtr(retVal, expectedType, "retcast");
                } else if (expectedType->isIntegerTy() && retVal->getType()->isPointerTy()) {
                    retVal = builder_->CreatePtrToInt(retVal, expectedType, "retcast");
                }
            }
            builder_->CreateRet(retVal);
        }
    } else {
        builder_->CreateRetVoid();
    }
}

void IRGenerator::genExprStmt(const ExpressionStmt& stmt) {
    if (stmt.expression) genExpr(*stmt.expression);
}

void IRGenerator::genCompoundStmt(const CompoundStmt& stmt) {
    for (const auto& s : stmt.statements) {
        if (s) genStmt(*s);
    }
}

void IRGenerator::genIfStmt(const IfDecl& decl) {
    llvm::Value* cond = genExpr(*decl.condition);
    if (!cond) return;

    // 转换为 bool / Convert to bool
    if (!cond->getType()->isIntegerTy(1)) {
        cond = builder_->CreateICmpNE(cond,
            llvm::ConstantInt::get(cond->getType(), 0), "tobool");
    }

    llvm::Function* func = builder_->GetInsertBlock()->getParent();
    llvm::BasicBlock* thenBB = llvm::BasicBlock::Create(context_, "if.then", func);
    llvm::BasicBlock* elseBB = decl.elseBody.empty() ? nullptr :
        llvm::BasicBlock::Create(context_, "if.else", func);
    llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(context_, "if.end", func);

    if (elseBB) {
        builder_->CreateCondBr(cond, thenBB, elseBB);
    } else {
        builder_->CreateCondBr(cond, thenBB, mergeBB);
    }

    // Then 分支
    builder_->SetInsertPoint(thenBB);
    for (const auto& s : decl.thenBody) {
        if (s) genStmt(*s);
    }
    if (!builder_->GetInsertBlock()->getTerminator()) {
        builder_->CreateBr(mergeBB);
    }

    // Else 分支
    if (elseBB) {
        builder_->SetInsertPoint(elseBB);
        for (const auto& s : decl.elseBody) {
            if (s) genStmt(*s);
        }
        if (!builder_->GetInsertBlock()->getTerminator()) {
            builder_->CreateBr(mergeBB);
        }
    }

    builder_->SetInsertPoint(mergeBB);
}

void IRGenerator::genWhileStmt(const WhileDecl& decl) {
    llvm::Function* func = builder_->GetInsertBlock()->getParent();
    llvm::BasicBlock* condBB = llvm::BasicBlock::Create(context_, "while.cond", func);
    llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(context_, "while.body", func);
    llvm::BasicBlock* endBB = llvm::BasicBlock::Create(context_, "while.end", func);

    loopStack_.push_back({condBB, endBB});

    builder_->CreateBr(condBB);
    builder_->SetInsertPoint(condBB);
    llvm::Value* cond = genExpr(*decl.condition);
    if (cond && !cond->getType()->isIntegerTy(1)) {
        cond = builder_->CreateICmpNE(cond,
            llvm::ConstantInt::get(cond->getType(), 0), "tobool");
    }
    builder_->CreateCondBr(cond, bodyBB, endBB);

    builder_->SetInsertPoint(bodyBB);
    for (const auto& s : decl.body) {
        if (s) genStmt(*s);
    }
    if (!builder_->GetInsertBlock()->getTerminator()) {
        builder_->CreateBr(condBB);
    }

    builder_->SetInsertPoint(endBB);
    loopStack_.pop_back();
}

void IRGenerator::genForInStmt(const ForInDecl& decl) {
    // 简化实现：将 for-in 展开为 while 循环
    // TODO: 正确的迭代器协议实现
    if (!decl.body.empty()) {
        for (const auto& s : decl.body) {
            if (s) genStmt(*s);
        }
    }
}

void IRGenerator::genSwitchStmt(const SwitchDecl& decl) {
    llvm::Value* subject = genExpr(*decl.subject);
    if (!subject) return;

    llvm::Function* func = builder_->GetInsertBlock()->getParent();
    llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(context_, "switch.end", func);

    // 生成 case 比较
    std::vector<llvm::BasicBlock*> caseBBs;
    llvm::BasicBlock* defaultBB = nullptr;

    for (size_t i = 0; i < decl.cases.size(); i++) {
        auto& sc = decl.cases[i];
        bool isDefault = false;
        for (const auto& label : sc.labels) {
            if (label.isDefault) isDefault = true;
        }
        if (isDefault) {
            defaultBB = llvm::BasicBlock::Create(context_, "switch.default", func);
        } else {
            caseBBs.push_back(llvm::BasicBlock::Create(context_, "switch.case", func));
        }
    }
    if (!defaultBB) defaultBB = mergeBB;

    // 生成条件分支
    size_t caseIdx = 0;
    for (size_t i = 0; i < decl.cases.size(); i++) {
        auto& sc = decl.cases[i];
        bool isDefault = false;
        for (const auto& label : sc.labels) {
            if (label.isDefault) isDefault = true;
        }
        if (isDefault) continue;

        if (caseIdx < caseBBs.size() && sc.labels[0].expression) {
            llvm::Value* caseVal = genExpr(*sc.labels[0].expression);
            if (caseVal) {
                llvm::Value* cmp = builder_->CreateICmpEQ(subject, caseVal, "casecmp");
                llvm::BasicBlock* nextBB = (caseIdx + 1 < caseBBs.size()) ?
                    caseBBs[caseIdx + 1] : defaultBB;
                builder_->CreateCondBr(cmp, caseBBs[caseIdx], nextBB);
                builder_->SetInsertPoint(caseBBs[caseIdx]);
                for (const auto& s : sc.body) {
                    if (s) genStmt(*s);
                }
                if (!builder_->GetInsertBlock()->getTerminator()) {
                    builder_->CreateBr(mergeBB);
                }
            }
        }
        caseIdx++;
    }

    // Default 分支
    if (defaultBB != mergeBB) {
        builder_->SetInsertPoint(defaultBB);
        for (auto& sc : decl.cases) {
            for (const auto& label : sc.labels) {
                if (label.isDefault) {
                    for (const auto& s : sc.body) {
                        if (s) genStmt(*s);
                    }
                }
            }
        }
        if (!builder_->GetInsertBlock()->getTerminator()) {
            builder_->CreateBr(mergeBB);
        }
    }

    builder_->SetInsertPoint(mergeBB);
}

void IRGenerator::genDoCatchStmt(const DoCatchDecl& decl) {
    // 简化实现：只执行 do 块（不做异常处理）
    for (const auto& s : decl.doBody) {
        if (s) genStmt(*s);
    }
}

void IRGenerator::genThrowStmt(const ThrowDecl& decl) {
    // 简化实现：调用 abort
    llvm::Function* abortFunc = module_->getFunction("abort");
    if (!abortFunc) {
        llvm::FunctionType* abortTy = llvm::FunctionType::get(
            llvm::Type::getVoidTy(context_), false);
        abortFunc = llvm::Function::Create(abortTy, llvm::Function::ExternalLinkage,
                                           "abort", module_.get());
    }
    builder_->CreateCall(abortFunc);
    builder_->CreateUnreachable();
}

// ─── 表达式代码生成 / Expression codegen ───────────────────────────────

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
        case ExprKind::NilLiteral:
            return genNilLiteral();
        case ExprKind::Identifier:
            return genIdentifier(static_cast<const IdentifierExpr&>(expr));
        case ExprKind::Binary:
            return genBinaryExpr(static_cast<const BinaryExpr&>(expr));
        case ExprKind::Unary:
            return genUnaryExpr(static_cast<const UnaryExpr&>(expr));
        case ExprKind::Call:
            return genCallExpr(static_cast<const CallExpr&>(expr));
        case ExprKind::MemberAccess:
            return genMemberAccess(static_cast<const MemberAccessExpr&>(expr));
        case ExprKind::ArrayLiteral:
            return genArrayLiteral(static_cast<const ArrayLiteralExpr&>(expr));
        case ExprKind::DictLiteral:
            return genDictLiteral(static_cast<const DictLiteralExpr&>(expr));
        case ExprKind::Tuple:
            return genTupleExpr(static_cast<const TupleExpr&>(expr));
        case ExprKind::If:
            return genIfExpr(static_cast<const IfExpr&>(expr));
        case ExprKind::InterpolatedString:
            return genInterpolatedString(static_cast<const InterpolatedStringExpr&>(expr));
        case ExprKind::TypeCast: {
            auto& tc = static_cast<const TypeCastExpr&>(expr);
            llvm::Value* val = genExpr(*tc.subExpr);
            if (!val) return nullptr;
            llvm::Type* targetType = resolveType(tc.targetType.get());
            if (val->getType() == targetType) return val;
            // Int -> Float
            if (targetType->isDoubleTy() && val->getType()->isIntegerTy()) {
                return builder_->CreateSIToFP(val, targetType, "cast");
            }
            // Float -> Int
            if (targetType->isIntegerTy() && val->getType()->isFloatingPointTy()) {
                return builder_->CreateFPToSI(val, targetType, "cast");
            }
            // Int width conversion
            if (targetType->isIntegerTy() && val->getType()->isIntegerTy()) {
                if (targetType->getIntegerBitWidth() > val->getType()->getIntegerBitWidth()) {
                    return builder_->CreateSExt(val, targetType, "cast");
                }
                return builder_->CreateTrunc(val, targetType, "cast");
            }
            return val;
        }
        case ExprKind::TypeCheck:
            return llvm::ConstantInt::getTrue(context_); // 简化：总是返回 true
        case ExprKind::Await:
        case ExprKind::Try: {
            auto* sub = (expr.exprKind == ExprKind::Await) ?
                static_cast<const AwaitExpr&>(expr).subExpr.get() :
                static_cast<const TryExpr&>(expr).subExpr.get();
            return genExpr(*sub);
        }
        case ExprKind::Move: {
            auto& me = static_cast<const MoveExpr&>(expr);
            return genExpr(*me.subExpr);
        }
        case ExprKind::InOut: {
            auto& ie = static_cast<const InOutExpr&>(expr);
            if (ie.subExpr->exprKind == ExprKind::Identifier) {
                auto& id = static_cast<const IdentifierExpr&>(*ie.subExpr);
                auto it = namedValues_.find(id.name);
                if (it != namedValues_.end()) return it->second;
            }
            return genExpr(*ie.subExpr);
        }
        case ExprKind::SelfRef:
        case ExprKind::SuperRef:
            return nullptr; // TODO
        case ExprKind::ForceUnwrap: {
            auto& fu = static_cast<const ForceUnwrapExpr&>(expr);
            return genExpr(*fu.subExpr); // 简化
        }
        case ExprKind::OptionalChain: {
            auto& oc = static_cast<const OptionalChainExpr&>(expr);
            return genExpr(*oc.subExpr); // 简化
        }
        case ExprKind::Subscript: {
            auto& sub = static_cast<const SubscriptExpr&>(expr);
            llvm::Value* base = genExpr(*sub.base);
            if (!base || sub.indices.empty()) return nullptr;
            llvm::Value* index = genExpr(*sub.indices[0]);
            if (!index) return nullptr;
            // 简化：计算指针偏移 / Simplified: compute pointer offset
            return builder_->CreateGEP(llvm::Type::getInt8Ty(context_), base, index, "idx");
        }
        case ExprKind::Closure:
            return nullptr; // TODO: 闭包代码生成
        case ExprKind::CharLiteral: {
            auto& ch = static_cast<const CharLiteralExpr&>(expr);
            return llvm::ConstantInt::get(llvm::Type::getInt32Ty(context_), ch.value);
        }
        case ExprKind::Assignment: {
            auto& ae = static_cast<const AssignmentExpr&>(expr);
            llvm::Value* val = genExpr(*ae.value);
            if (!val) return nullptr;
            if (ae.target->exprKind == ExprKind::Identifier) {
                auto& id = static_cast<const IdentifierExpr&>(*ae.target);
                auto it = namedValues_.find(id.name);
                if (it != namedValues_.end()) {
                    llvm::Type* varType = namedTypes_[id.name];

                    // 复合赋值运算符 / Compound assignment operators
                    if (ae.op != TokenKind::Assign) {
                        llvm::Value* current = builder_->CreateLoad(varType, it->second, "load");
                        llvm::Value* result = nullptr;
                        if (ae.op == TokenKind::PlusAssign) {
                            result = builder_->CreateAdd(current, val, "add");
                        } else if (ae.op == TokenKind::MinusAssign) {
                            result = builder_->CreateSub(current, val, "sub");
                        } else if (ae.op == TokenKind::StarAssign) {
                            result = builder_->CreateMul(current, val, "mul");
                        } else if (ae.op == TokenKind::SlashAssign) {
                            result = builder_->CreateSDiv(current, val, "div");
                        } else if (ae.op == TokenKind::PercentAssign) {
                            result = builder_->CreateSRem(current, val, "mod");
                        } else if (ae.op == TokenKind::AmpAssign) {
                            result = builder_->CreateAnd(current, val, "and");
                        } else if (ae.op == TokenKind::PipeAssign) {
                            result = builder_->CreateOr(current, val, "or");
                        } else if (ae.op == TokenKind::CaretAssign) {
                            result = builder_->CreateXor(current, val, "xor");
                        } else if (ae.op == TokenKind::LShiftAssign) {
                            result = builder_->CreateShl(current, val, "shl");
                        } else if (ae.op == TokenKind::RShiftAssign) {
                            result = builder_->CreateAShr(current, val, "shr");
                        }
                        if (result) {
                            builder_->CreateStore(result, it->second);
                            return result;
                        }
                    }

                    // 简单赋值 / Simple assignment
                    if (val->getType() != varType) {
                        if (varType->isDoubleTy() && val->getType()->isIntegerTy()) {
                            val = builder_->CreateSIToFP(val, varType, "cast");
                        }
                    }
                    builder_->CreateStore(val, it->second);
                    return val;
                }
            }
            return val;
        }
        default:
            return nullptr;
    }
}

llvm::Value* IRGenerator::genIntegerLiteral(const IntegerLiteralExpr& expr,
                                             llvm::Type* expectedType) {
    llvm::Type* ty = expectedType ? expectedType : llvm::Type::getInt64Ty(context_);
    if (!ty->isIntegerTy()) ty = llvm::Type::getInt64Ty(context_);
    return llvm::ConstantInt::get(ty, expr.value, true);
}

llvm::Value* IRGenerator::genFloatLiteral(const FloatLiteralExpr& expr) {
    return llvm::ConstantFP::get(context_, llvm::APFloat(expr.value));
}

llvm::Value* IRGenerator::genStringLiteral(const StringLiteralExpr& expr) {
    return createStringGlobal(expr.value);
}

llvm::Value* IRGenerator::genBoolLiteral(const BoolLiteralExpr& expr) {
    return llvm::ConstantInt::get(llvm::Type::getInt1Ty(context_), expr.value ? 1 : 0);
}

llvm::Value* IRGenerator::genNilLiteral() {
    return llvm::ConstantPointerNull::get(llvm::PointerType::get(context_, 0));
}

llvm::Value* IRGenerator::genIdentifier(const IdentifierExpr& expr) {
    auto it = namedValues_.find(expr.name);
    if (it != namedValues_.end()) {
        llvm::Type* varType = namedTypes_[expr.name];
        return builder_->CreateLoad(varType, it->second, expr.name);
    }
    auto funcIt = functions_.find(expr.name);
    if (funcIt != functions_.end()) return funcIt->second;
    error(expr.loc, "unknown identifier: " + expr.name);
    return nullptr;
}

llvm::Value* IRGenerator::genBinaryExpr(const BinaryExpr& expr) {
    // 短路求值 / Short-circuit evaluation
    if (expr.op == TokenKind::AmpAmp) {
        llvm::Value* lhs = genExpr(*expr.left);
        if (!lhs) return nullptr;
        llvm::Function* func = builder_->GetInsertBlock()->getParent();
        llvm::BasicBlock* rhsBB = llvm::BasicBlock::Create(context_, "and.rhs", func);
        llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(context_, "and.merge", func);
        builder_->CreateCondBr(lhs, rhsBB, mergeBB);
        builder_->SetInsertPoint(rhsBB);
        llvm::Value* rhs = genExpr(*expr.right);
        builder_->CreateBr(mergeBB);
        rhsBB = builder_->GetInsertBlock();
        builder_->SetInsertPoint(mergeBB);
        llvm::PHINode* phi = builder_->CreatePHI(llvm::Type::getInt1Ty(context_), 2, "and");
        phi->addIncoming(llvm::ConstantInt::getFalse(context_), /*from*/ builder_->GetInsertBlock());
        phi->addIncoming(rhs, rhsBB);
        return phi;
    }
    if (expr.op == TokenKind::PipePipe) {
        llvm::Value* lhs = genExpr(*expr.left);
        if (!lhs) return nullptr;
        llvm::Function* func = builder_->GetInsertBlock()->getParent();
        llvm::BasicBlock* rhsBB = llvm::BasicBlock::Create(context_, "or.rhs", func);
        llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(context_, "or.merge", func);
        builder_->CreateCondBr(lhs, mergeBB, rhsBB);
        builder_->SetInsertPoint(rhsBB);
        llvm::Value* rhs = genExpr(*expr.right);
        builder_->CreateBr(mergeBB);
        rhsBB = builder_->GetInsertBlock();
        builder_->SetInsertPoint(mergeBB);
        llvm::PHINode* phi = builder_->CreatePHI(llvm::Type::getInt1Ty(context_), 2, "or");
        phi->addIncoming(llvm::ConstantInt::getTrue(context_), /*from*/ builder_->GetInsertBlock());
        phi->addIncoming(rhs, rhsBB);
        return phi;
    }

    llvm::Value* left = genExpr(*expr.left);
    llvm::Value* right = genExpr(*expr.right);
    if (!left || !right) return nullptr;

    // 类型提升 / Type promotion
    if (left->getType() != right->getType()) {
        if (left->getType()->isDoubleTy() && right->getType()->isIntegerTy()) {
            right = builder_->CreateSIToFP(right, left->getType(), "promote");
        } else if (right->getType()->isDoubleTy() && left->getType()->isIntegerTy()) {
            left = builder_->CreateSIToFP(left, right->getType(), "promote");
        } else if (left->getType()->isIntegerTy() && right->getType()->isIntegerTy()) {
            if (left->getType()->getIntegerBitWidth() < right->getType()->getIntegerBitWidth()) {
                left = builder_->CreateSExt(left, right->getType(), "promote");
            } else if (right->getType()->getIntegerBitWidth() < left->getType()->getIntegerBitWidth()) {
                right = builder_->CreateSExt(right, left->getType(), "promote");
            }
        }
    }

    bool isFloat = left->getType()->isFloatingPointTy();

    switch (expr.op) {
        case TokenKind::Plus:
            return isFloat ? builder_->CreateFAdd(left, right, "fadd") : builder_->CreateAdd(left, right, "add");
        case TokenKind::Minus:
            return isFloat ? builder_->CreateFSub(left, right, "fsub") : builder_->CreateSub(left, right, "sub");
        case TokenKind::Star:
            return isFloat ? builder_->CreateFMul(left, right, "fmul") : builder_->CreateMul(left, right, "mul");
        case TokenKind::Slash:
            return isFloat ? builder_->CreateFDiv(left, right, "fdiv") : builder_->CreateSDiv(left, right, "div");
        case TokenKind::Percent:
            return isFloat ? builder_->CreateFRem(left, right, "frem") : builder_->CreateSRem(left, right, "mod");
        case TokenKind::Equal:
            return isFloat ? builder_->CreateFCmpOEQ(left, right, "feq") : builder_->CreateICmpEQ(left, right, "eq");
        case TokenKind::NotEqual:
            return isFloat ? builder_->CreateFCmpONE(left, right, "fne") : builder_->CreateICmpNE(left, right, "ne");
        case TokenKind::Less:
            return isFloat ? builder_->CreateFCmpOLT(left, right, "flt") : builder_->CreateICmpSLT(left, right, "slt");
        case TokenKind::Greater:
            return isFloat ? builder_->CreateFCmpOGT(left, right, "fgt") : builder_->CreateICmpSGT(left, right, "sgt");
        case TokenKind::LessEqual:
            return isFloat ? builder_->CreateFCmpOLE(left, right, "fle") : builder_->CreateICmpSLE(left, right, "sle");
        case TokenKind::GreaterEqual:
            return isFloat ? builder_->CreateFCmpOGE(left, right, "fge") : builder_->CreateICmpSGE(left, right, "sge");
        case TokenKind::Amp:    return builder_->CreateAnd(left, right, "and");
        case TokenKind::Pipe:   return builder_->CreateOr(left, right, "or");
        case TokenKind::Caret:  return builder_->CreateXor(left, right, "xor");
        case TokenKind::LShift: return builder_->CreateShl(left, right, "shl");
        case TokenKind::RShift: return builder_->CreateAShr(left, right, "shr");
        default:
            error({}, "unsupported binary operator");
            return nullptr;
    }
}

llvm::Value* IRGenerator::genUnaryExpr(const UnaryExpr& expr) {
    llvm::Value* operand = genExpr(*expr.operand);
    if (!operand) return nullptr;

    switch (expr.op) {
        case TokenKind::Minus:
            if (operand->getType()->isFloatingPointTy()) {
                return builder_->CreateFNeg(operand, "fneg");
            }
            return builder_->CreateNeg(operand, "neg");
        case TokenKind::Bang:
            return builder_->CreateNot(operand, "not");
        case TokenKind::Tilde:
            return builder_->CreateNot(operand, "bnot");
        default:
            return operand;
    }
}

llvm::Value* IRGenerator::genCallExpr(const CallExpr& expr) {
    if (expr.callee->exprKind != ExprKind::Identifier) {
        error({}, "only direct function calls supported");
        return nullptr;
    }

    auto& funcName = static_cast<const IdentifierExpr&>(*expr.callee).name;

    // 内建 print 函数 / Built-in print function
    if (funcName == "print") {
        // 查找或声明 printf
        llvm::Function* printfFunc = module_->getFunction("printf");
        if (!printfFunc) {
            llvm::Type* printfArgTy = llvm::PointerType::get(context_, 0);
            llvm::FunctionType* printfTy = llvm::FunctionType::get(
                llvm::Type::getInt32Ty(context_), {printfArgTy}, true);
            printfFunc = llvm::Function::Create(printfTy, llvm::Function::ExternalLinkage,
                                                "printf", module_.get());
        }

        if (!expr.args.empty()) {
            llvm::Value* arg = genExpr(*expr.args[0].value);
            if (!arg) return nullptr;

            if (arg->getType()->isIntegerTy(64)) {
                // Int: 使用 %lld 格式
                llvm::Value* fmt = createStringGlobal("%lld\n");
                return builder_->CreateCall(printfFunc, {fmt, arg}, "printf");
            } else if (arg->getType()->isIntegerTy(1)) {
                // Bool: 使用 %s 格式
                llvm::Value* trueStr = createStringGlobal("true");
                llvm::Value* falseStr = createStringGlobal("false");
                llvm::Value* str = builder_->CreateSelect(arg, trueStr, falseStr);
                llvm::Value* fmt = createStringGlobal("%s\n");
                return builder_->CreateCall(printfFunc, {fmt, str}, "printf");
            } else if (arg->getType()->isDoubleTy()) {
                // Double: 使用 %g 格式
                llvm::Value* fmt = createStringGlobal("%g\n");
                return builder_->CreateCall(printfFunc, {fmt, arg}, "printf");
            } else if (arg->getType()->isPointerTy()) {
                // String: 使用 %s 格式
                llvm::Value* fmt = createStringGlobal("%s\n");
                return builder_->CreateCall(printfFunc, {fmt, arg}, "printf");
            }
        }
        return nullptr;
    }

    llvm::Function* func = module_->getFunction(funcName);

    // 泛型函数单态化 / Generic function monomorphization
    if (!func || genericFuncAsts_.count(funcName) > 0) {
        // 生成参数类型列表以创建特化函数名
        std::vector<llvm::Type*> argTypes;
        for (const auto& arg : expr.args) {
            llvm::Value* argVal = genExpr(*arg.value);
            if (argVal) argTypes.push_back(argVal->getType());
        }

        // 创建特化函数名: funcName<Type1,Type2,...>
        std::string specName = funcName;
        for (auto* ty : argTypes) {
            specName += "_";
            if (ty->isIntegerTy(64)) specName += "i64";
            else if (ty->isIntegerTy(32)) specName += "i32";
            else if (ty->isDoubleTy()) specName += "f64";
            else if (ty->isFloatTy()) specName += "f32";
            else if (ty->isPointerTy()) specName += "ptr";
            else specName += "any";
        }

        // 检查特化函数是否已存在
        llvm::Function* specFunc = module_->getFunction(specName);
        if (!specFunc && genericFuncAsts_.count(funcName) > 0) {
            // 生成特化函数 / Generate specialized function
            const FunctionDecl* genericAst = genericFuncAsts_[funcName];
            std::vector<llvm::Type*> paramTypes;
            for (size_t i = 0; i < genericAst->params.size(); i++) {
                if (i < argTypes.size()) {
                    paramTypes.push_back(argTypes[i]);
                } else {
                    paramTypes.push_back(resolveType(genericAst->params[i].type.get()));
                }
            }
            llvm::Type* retType = !argTypes.empty() ? argTypes[0] :
                (genericAst->returnType ? resolveType(genericAst->returnType.get()) :
                 llvm::Type::getVoidTy(context_));

            llvm::FunctionType* funcType = llvm::FunctionType::get(retType, paramTypes, false);
            specFunc = llvm::Function::Create(
                funcType, llvm::Function::ExternalLinkage, specName, module_.get());

            // 设置参数名称
            size_t aIdx = 0;
            for (auto& a : specFunc->args()) {
                if (aIdx < genericAst->params.size()) {
                    a.setName(genericAst->params[aIdx].internalName);
                }
                aIdx++;
            }

            // 生成函数体 / Generate function body
            llvm::BasicBlock* entry = llvm::BasicBlock::Create(context_, "entry", specFunc);
            builder_->SetInsertPoint(entry);

            llvm::Function* prevFunc = currentFunc_;
            currentFunc_ = specFunc;

            // 保存和恢复命名值 / Save and restore named values
            auto savedValues = namedValues_;
            auto savedTypes = namedTypes_;
            namedValues_.clear();
            namedTypes_.clear();

            // 为参数创建 alloca
            size_t idx = 0;
            for (auto& arg : specFunc->args()) {
                llvm::Type* pType = arg.getType();
                llvm::AllocaInst* allocaInst = createEntryBlockAlloca(specFunc, pType, std::string(arg.getName()));
                builder_->CreateStore(&arg, allocaInst);
                namedValues_[std::string(arg.getName())] = allocaInst;
                namedTypes_[std::string(arg.getName())] = pType;
                idx++;
            }

            // 生成函数体
            for (const auto& stmt : genericAst->body) {
                if (stmt) genStmt(*stmt);
            }

            // 添加 return
            if (!builder_->GetInsertBlock()->getTerminator()) {
                if (retType->isVoidTy()) {
                    builder_->CreateRetVoid();
                } else {
                    builder_->CreateRet(llvm::Constant::getNullValue(retType));
                }
            }

            llvm::verifyFunction(*specFunc);
            currentFunc_ = prevFunc;
            namedValues_ = savedValues;
            namedTypes_ = savedTypes;

            // 恢复插入点 / Restore insert point
            if (currentFunc_ && !currentFunc_->empty()) {
                builder_->SetInsertPoint(&currentFunc_->back());
            }
        }

        if (specFunc) func = specFunc;
    }

    if (!func) {
        error({}, "unknown function: " + funcName);
        return nullptr;
    }

    std::vector<llvm::Value*> args;
    for (const auto& arg : expr.args) {
        llvm::Value* argVal = genExpr(*arg.value);
        if (!argVal) return nullptr;

        // 参数类型转换 / Argument type conversion
        size_t argIdx = args.size();
        if (argIdx < func->arg_size()) {
            llvm::Type* expectedType = func->getArg(argIdx)->getType();
            if (argVal->getType() != expectedType) {
                if (expectedType->isDoubleTy() && argVal->getType()->isIntegerTy()) {
                    argVal = builder_->CreateSIToFP(argVal, expectedType, "argcast");
                } else if (expectedType->isIntegerTy(64) && argVal->getType()->isIntegerTy()) {
                    argVal = builder_->CreateSExt(argVal, expectedType, "argcast");
                } else if (expectedType->isPointerTy() && argVal->getType()->isIntegerTy()) {
                    argVal = builder_->CreateIntToPtr(argVal, expectedType, "argcast");
                } else if (expectedType->isIntegerTy() && argVal->getType()->isPointerTy()) {
                    argVal = builder_->CreatePtrToInt(argVal, expectedType, "argcast");
                }
            }
        }

        args.push_back(argVal);
    }

    if (args.size() != func->arg_size()) {
        error({}, "wrong number of arguments for " + funcName);
        return nullptr;
    }

    return builder_->CreateCall(func, args, func->getReturnType()->isVoidTy() ? "" : "call");
}

llvm::Value* IRGenerator::genMemberAccess(const MemberAccessExpr& expr) {
    // TODO: 基于基类型的成员查找
    return nullptr;
}

llvm::Value* IRGenerator::genArrayLiteral(const ArrayLiteralExpr& expr) {
    // 简化：创建全局数组并返回指针
    // TODO: 正确的 Array<T> 运行时表示
    if (expr.elements.empty()) return llvm::ConstantPointerNull::get(llvm::PointerType::get(context_, 0));
    return genExpr(*expr.elements[0]); // 简化：返回第一个元素
}

llvm::Value* IRGenerator::genDictLiteral(const DictLiteralExpr& expr) {
    return llvm::ConstantPointerNull::get(llvm::PointerType::get(context_, 0));
}

llvm::Value* IRGenerator::genTupleExpr(const TupleExpr& expr) {
    if (expr.elements.empty()) return llvm::ConstantStruct::getAnon(context_, {});
    // 简化：返回第一个元素
    return genExpr(*expr.elements[0].value);
}

llvm::Value* IRGenerator::genIfExpr(const IfExpr& expr) {
    llvm::Value* cond = genExpr(*expr.condition);
    if (!cond) return nullptr;
    if (!cond->getType()->isIntegerTy(1)) {
        cond = builder_->CreateICmpNE(cond, llvm::ConstantInt::get(cond->getType(), 0), "tobool");
    }

    llvm::Function* func = builder_->GetInsertBlock()->getParent();
    llvm::BasicBlock* thenBB = llvm::BasicBlock::Create(context_, "if.then", func);
    llvm::BasicBlock* elseBB = llvm::BasicBlock::Create(context_, "if.else", func);
    llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(context_, "if.merge", func);

    builder_->CreateCondBr(cond, thenBB, elseBB);

    builder_->SetInsertPoint(thenBB);
    llvm::Value* thenVal = genExpr(*expr.thenExpr);
    builder_->CreateBr(mergeBB);
    thenBB = builder_->GetInsertBlock();

    builder_->SetInsertPoint(elseBB);
    llvm::Value* elseVal = expr.elseExpr ? genExpr(*expr.elseExpr) : llvm::Constant::getNullValue(thenVal->getType());
    builder_->CreateBr(mergeBB);
    elseBB = builder_->GetInsertBlock();

    builder_->SetInsertPoint(mergeBB);
    llvm::PHINode* phi = builder_->CreatePHI(thenVal->getType(), 2, "if");
    phi->addIncoming(thenVal, thenBB);
    phi->addIncoming(elseVal, elseBB);
    return phi;
}

llvm::Value* IRGenerator::genInterpolatedString(const InterpolatedStringExpr& expr) {
    // 简化：连接所有片段
    // TODO: 正确的字符串拼接
    std::string result;
    for (const auto& seg : expr.segments) {
        result += seg.literalText;
    }
    return createStringGlobal(result);
}

// ─── 辅助 / Helpers ────────────────────────────────────────────────────

llvm::AllocaInst* IRGenerator::createEntryBlockAlloca(llvm::Function* func,
                                                       llvm::Type* type,
                                                       const std::string& name) {
    llvm::IRBuilder<> tmpB(&func->getEntryBlock(), func->getEntryBlock().begin());
    return tmpB.CreateAlloca(type, nullptr, name);
}

llvm::Value* IRGenerator::createStringGlobal(const std::string& str) {
    llvm::Constant* strConst = llvm::ConstantDataArray::getString(context_, str);
    llvm::GlobalVariable* global = new llvm::GlobalVariable(
        *module_, strConst->getType(), true,
        llvm::GlobalValue::PrivateLinkage, strConst, ".str");
    return builder_->CreatePointerCast(global, llvm::PointerType::get(context_, 0));
}

void IRGenerator::error(SourceLocation loc, const std::string& msg) {
    diag_.error(loc, moduleName_, msg);
}

// ─── ARC 支持 / ARC support ─────────────────────────────────────────────

void IRGenerator::insertRetain(llvm::Value* obj) {
    if (!obj || !isReferenceType(obj->getType())) return;

    // 查找或声明 suki_retain 函数
    llvm::Function* retainFunc = module_->getFunction("suki_retain");
    if (!retainFunc) {
        llvm::FunctionType* retainTy = llvm::FunctionType::get(
            llvm::Type::getVoidTy(context_),
            {llvm::PointerType::get(context_, 0)}, false);
        retainFunc = llvm::Function::Create(retainTy, llvm::Function::ExternalLinkage,
                                            "suki_retain", module_.get());
    }

    // 将对象指针传给 retain
    llvm::Value* ptr = obj;
    if (!obj->getType()->isPointerTy()) {
        ptr = builder_->CreateIntToPtr(obj, llvm::PointerType::get(context_, 0));
    }
    builder_->CreateCall(retainFunc, {ptr});
}

void IRGenerator::insertRelease(llvm::Value* obj) {
    if (!obj || !isReferenceType(obj->getType())) return;

    // 查找或声明 suki_release 函数
    llvm::Function* releaseFunc = module_->getFunction("suki_release");
    if (!releaseFunc) {
        llvm::FunctionType* releaseTy = llvm::FunctionType::get(
            llvm::Type::getVoidTy(context_),
            {llvm::PointerType::get(context_, 0)}, false);
        releaseFunc = llvm::Function::Create(releaseTy, llvm::Function::ExternalLinkage,
                                             "suki_release", module_.get());
    }

    llvm::Value* ptr = obj;
    if (!obj->getType()->isPointerTy()) {
        ptr = builder_->CreateIntToPtr(obj, llvm::PointerType::get(context_, 0));
    }
    builder_->CreateCall(releaseFunc, {ptr});
}

bool IRGenerator::isReferenceType(llvm::Type* type) const {
    // 指针类型视为引用类型
    return type->isPointerTy();
}

#endif

} // namespace suki
