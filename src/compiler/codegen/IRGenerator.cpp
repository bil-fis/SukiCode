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
    : diag_(diag), moduleName_(moduleName), macroExpander_(diag)
#ifdef SUKI_HAS_LLVM
      , context_()
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
        if (!decl) continue;

        // 收集需要预注册的函数 / Collect functions to pre-register
        std::vector<const FunctionDecl*> funcsToRegister;

        if (decl->declKind == DeclKind::Function) {
            funcsToRegister.push_back(static_cast<const FunctionDecl*>(decl.get()));
        } else if (decl->declKind == DeclKind::ExternBlock) {
            // extern 块中的函数也需要预注册
            auto& eb = static_cast<const ExternBlockDecl&>(*decl);
            for (const auto& d : eb.declarations) {
                if (d && d->declKind == DeclKind::Function) {
                    funcsToRegister.push_back(static_cast<const FunctionDecl*>(d.get()));
                }
            }
        }

        for (const auto& fdPtr : funcsToRegister) {
            const auto& fd = *fdPtr;

            // 存储泛型函数 AST / Store generic function AST
            if (!fd.genericParams.empty()) {
                genericFuncAsts_[fd.name] = &fd;
                // 泛型函数在预注册时使用 i64 作为占位符
                // Generic functions use i64 as placeholder during pre-registration
            }

            std::vector<llvm::Type*> paramTypes;
            for (const auto& param : fd.params) {
                llvm::Type* pType = resolveType(param.type.get());
                // inout 参数使用指针类型 / inout parameters use pointer type
                if (param.isInOut) {
                    pType = llvm::PointerType::get(context_, 0);
                }
                paramTypes.push_back(pType);
            }
            llvm::Type* retType = fd.returnType ?
                resolveType(fd.returnType.get()) :
                llvm::Type::getVoidTy(context_);
            llvm::FunctionType* funcType = llvm::FunctionType::get(retType, paramTypes, fd.isVariadic);
            // 根据访问级别设置链接类型 / Set linkage based on access level
            llvm::GlobalValue::LinkageTypes linkage = llvm::Function::ExternalLinkage;
            if (fd.access == AccessLevel::Private || fd.access == AccessLevel::FilePrivate) {
                linkage = llvm::Function::InternalLinkage;
            }
            llvm::Function* func = llvm::Function::Create(
                funcType, linkage, fd.name, module_.get());
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

    // 预声明 ObjC 运行时函数 / Pre-declare ObjC runtime functions
    // 这些函数在链接时由 libobjc 提供
    {
        llvm::Type* ptrTy = llvm::PointerType::get(context_, 0);
        llvm::Type* voidTy = llvm::Type::getVoidTy(context_);
        llvm::Type* i32Ty = llvm::Type::getInt32Ty(context_);

        // id objc_msgSend(id self, SEL _cmd, ...)
        auto* objcMsgSendTy = llvm::FunctionType::get(ptrTy, {ptrTy, ptrTy}, true);
        llvm::Function::Create(objcMsgSendTy, llvm::Function::ExternalLinkage,
                               "objc_msgSend", module_.get());

        // id objc_msgSend_stret(id self, SEL _cmd, ...)
        auto* objcMsgSendStretTy = llvm::FunctionType::get(voidTy, {ptrTy, ptrTy}, true);
        llvm::Function::Create(objcMsgSendStretTy, llvm::Function::ExternalLinkage,
                               "objc_msgSend_stret", module_.get());

        // SEL sel_registerName(const char *str)
        auto* selRegisterNameTy = llvm::FunctionType::get(ptrTy, {ptrTy}, false);
        llvm::Function::Create(selRegisterNameTy, llvm::Function::ExternalLinkage,
                               "sel_registerName", module_.get());

        // Class objc_getClass(const char *name)
        auto* objcGetClassTy = llvm::FunctionType::get(ptrTy, {ptrTy}, false);
        llvm::Function::Create(objcGetClassTy, llvm::Function::ExternalLinkage,
                               "objc_getClass", module_.get());

        // Protocol *objc_getProtocol(const char *name)
        auto* objcGetProtocolTy = llvm::FunctionType::get(ptrTy, {ptrTy}, false);
        llvm::Function::Create(objcGetProtocolTy, llvm::Function::ExternalLinkage,
                               "objc_getProtocol", module_.get());

        // void objc_release(id obj)
        auto* objcReleaseTy = llvm::FunctionType::get(voidTy, {ptrTy}, false);
        llvm::Function::Create(objcReleaseTy, llvm::Function::ExternalLinkage,
                               "objc_release", module_.get());

        // id objc_retain(id obj)
        auto* objcRetainTy = llvm::FunctionType::get(ptrTy, {ptrTy}, false);
        llvm::Function::Create(objcRetainTy, llvm::Function::ExternalLinkage,
                               "objc_retain", module_.get());

        // void objc_storeStrong(id *location, id obj)
        auto* objcStoreStrongTy = llvm::FunctionType::get(voidTy, {ptrTy, ptrTy}, false);
        llvm::Function::Create(objcStoreStrongTy, llvm::Function::ExternalLinkage,
                               "objc_storeStrong", module_.get());

        // id objc_loadWeakRetained(id *location)
        auto* objcLoadWeakTy = llvm::FunctionType::get(ptrTy, {ptrTy}, false);
        llvm::Function::Create(objcLoadWeakTy, llvm::Function::ExternalLinkage,
                               "objc_loadWeakRetained", module_.get());

        // void objc_initWeak(id *location, id obj)
        auto* objcInitWeakTy = llvm::FunctionType::get(voidTy, {ptrTy, ptrTy}, false);
        llvm::Function::Create(objcInitWeakTy, llvm::Function::ExternalLinkage,
                               "objc_initWeak", module_.get());

        // void objc_destroyWeak(id *location)
        auto* objcDestroyWeakTy = llvm::FunctionType::get(voidTy, {ptrTy}, false);
        llvm::Function::Create(objcDestroyWeakTy, llvm::Function::ExternalLinkage,
                               "objc_destroyWeak", module_.get());

        // IMP class_getMethodImplementation(Class cls, SEL name)
        auto* classGetMethodTy = llvm::FunctionType::get(ptrTy, {ptrTy, ptrTy}, false);
        llvm::Function::Create(classGetMethodTy, llvm::Function::ExternalLinkage,
                               "class_getMethodImplementation", module_.get());

        // BOOL class_addMethod(Class cls, SEL name, IMP imp, const char *types)
        auto* classAddMethodTy = llvm::FunctionType::get(
            llvm::Type::getInt8Ty(context_), {ptrTy, ptrTy, ptrTy, ptrTy}, false);
        llvm::Function::Create(classAddMethodTy, llvm::Function::ExternalLinkage,
                               "class_addMethod", module_.get());

        // Class objc_allocateClassPair(Class superclass, const char *name, size_t extraBytes)
        auto* allocateClassTy = llvm::FunctionType::get(ptrTy, {ptrTy, ptrTy,
            llvm::Type::getInt64Ty(context_)}, false);
        llvm::Function::Create(allocateClassTy, llvm::Function::ExternalLinkage,
                               "objc_allocateClassPair", module_.get());

        // void objc_registerClassPair(Class cls)
        auto* registerClassTy = llvm::FunctionType::get(voidTy, {ptrTy}, false);
        llvm::Function::Create(registerClassTy, llvm::Function::ExternalLinkage,
                               "objc_registerClassPair", module_.get());
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
        // 检查是否有泛型参数 / Check for generic arguments
        if (!n.genericArgs.empty()) {
            // 生成特化类型名 / Generate specialized type name
            std::string specName = n.name + "<";
            for (size_t i = 0; i < n.genericArgs.size(); i++) {
                if (i > 0) specName += ",";
                if (n.genericArgs[i]->typeReprKind == TypeReprKind::Named) {
                    specName += static_cast<const NamedTypeRepr&>(*n.genericArgs[i]).name;
                } else {
                    specName += "?";
                }
            }
            specName += ">";
            // 检查是否已存在 / Check if already exists
            auto it = genericTypeInstances_.find(specName);
            if (it != genericTypeInstances_.end()) return it->second;
            // 查找原始泛型类型 / Find original generic type
            auto structIt = structTypes_.find(n.name);
            if (structIt != structTypes_.end()) {
                // 创建特化版本 / Create specialized version
                llvm::StructType* specType = llvm::StructType::create(context_, specName);

                // 收集泛型参数名和对应的具体类型
                std::vector<std::string> genericParamNames;
                std::vector<llvm::Type*> concreteTypes;
                for (size_t i = 0; i < n.genericArgs.size(); i++) {
                    concreteTypes.push_back(resolveType(n.genericArgs[i].get()));
                }

                // 查找原始结构体声明以获取泛型参数名
                if (currentCu_) {
                    for (const auto& d : currentCu_->declarations) {
                        if (d && d->declKind == DeclKind::Struct) {
                            auto& sd = static_cast<const StructDecl&>(*d);
                            if (sd.name == n.name) {
                                for (const auto& gp : sd.genericParams) {
                                    genericParamNames.push_back(gp.name);
                                }
                                break;
                            }
                        }
                    }
                }

                // 替换字段类型中的泛型参数
                std::vector<llvm::Type*> bodyTypes;
                for (size_t i = 0; i < structIt->second->getNumElements(); i++) {
                    llvm::Type* fieldType = structIt->second->getElementType(i);
                    // 检查字段类型名是否匹配泛型参数
                    if (fieldType->isStructTy()) {
                        std::string fieldTypeName = fieldType->getStructName().str();
                        for (size_t j = 0; j < genericParamNames.size(); j++) {
                            if (fieldTypeName == genericParamNames[j] && j < concreteTypes.size()) {
                                fieldType = concreteTypes[j];
                                break;
                            }
                        }
                    }
                    bodyTypes.push_back(fieldType);
                }
                specType->setBody(bodyTypes);
                genericTypeInstances_[specName] = specType;
                return specType;
            }
            // 如果找不到原始类型，返回默认
            return getLLVMType(n.name);
        }
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
        llvm::Type* baseType = resolveType(o.base.get());
        // Optional 使用 {value, hasValue} 标记结构体
        // Optional uses {value, hasValue} tagged struct (hasValue as i8 for alignment)
        std::string typeName = "Optional." + std::to_string(reinterpret_cast<uintptr_t>(o.base.get()));
        llvm::StructType* optType = llvm::StructType::getTypeByName(context_, typeName);
        if (!optType) {
            optType = llvm::StructType::create(context_, typeName);
            optType->setBody({baseType, llvm::Type::getInt8Ty(context_)});
        }
        return optType;
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
    if (tr->typeReprKind == TypeReprKind::Opaque) {
        // some Protocol: 解析约束类型 / Resolve constraint type
        auto& o = static_cast<const OpaqueTypeRepr&>(*tr);
        return resolveType(o.constraint.get());
    }
    if (tr->typeReprKind == TypeReprKind::Existential) {
        // any Protocol: 类型擦除为指针 / Type erase to pointer
        return llvm::PointerType::get(context_, 0);
    }
    if (tr->typeReprKind == TypeReprKind::Owned) {
        // Owned<T>: 与内部类型相同 / Same as inner type
        auto& o = static_cast<const OwnedTypeRepr&>(*tr);
        return resolveType(o.inner.get());
    }
    if (tr->typeReprKind == TypeReprKind::Self) {
        // Self: 返回当前类型 / Return current type
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
        case DeclKind::Struct: {
            auto& sd = static_cast<const StructDecl&>(decl);
            // 生成 LLVM struct 类型 / Generate LLVM struct type
            std::vector<llvm::Type*> fieldTypes;
            for (const auto& member : sd.members) {
                if (member && member->declKind == DeclKind::Variable) {
                    auto& vd = static_cast<const VariableDecl&>(*member);
                    fieldTypes.push_back(resolveType(vd.typeAnnotation.get()));
                }
            }
            llvm::StructType* structType = llvm::StructType::create(context_, sd.name);
            structType->setBody(fieldTypes);
            structTypes_[sd.name] = structType;
            break;
        }
        case DeclKind::Class: {
            auto& cd = static_cast<const ClassDecl&>(decl);
            // 设置父类名称 / Set superclass name
            std::string prevSuperclass = currentSuperclassName_;
            if (cd.superclass && cd.superclass->typeReprKind == TypeReprKind::Named) {
                currentSuperclassName_ = static_cast<const NamedTypeRepr&>(*cd.superclass).name;
            } else {
                currentSuperclassName_.clear();
            }
            // Class 生成指针类型（引用类型）
            llvm::StructType* classType = llvm::StructType::create(context_, cd.name);
            std::vector<llvm::Type*> fieldTypes;
            // 第一个字段是 vtable 指针 / First field is vtable pointer
            fieldTypes.push_back(llvm::PointerType::get(context_, 0));
            for (const auto& member : cd.members) {
                if (member && member->declKind == DeclKind::Variable) {
                    auto& vd = static_cast<const VariableDecl&>(*member);
                    fieldTypes.push_back(resolveType(vd.typeAnnotation.get()));
                }
            }
            classType->setBody(fieldTypes);
            structTypes_[cd.name] = classType;

            // 收集方法并生成 vtable / Collect methods and generate vtable
            VTableInfo vtable;
            std::vector<llvm::Type*> vtableFieldTypes;
            for (const auto& member : cd.members) {
                if (member && member->declKind == DeclKind::Function) {
                    auto& fd = static_cast<const FunctionDecl&>(*member);
                    // 生成函数类型 / Generate function type
                    std::vector<llvm::Type*> paramTypes;
                    paramTypes.push_back(llvm::PointerType::get(context_, 0)); // self
                    for (const auto& p : fd.params) {
                        paramTypes.push_back(resolveType(p.type.get()));
                    }
                    llvm::Type* retType = fd.returnType ?
                        resolveType(fd.returnType.get()) :
                        llvm::Type::getVoidTy(context_);
                    llvm::FunctionType* methodTy = llvm::FunctionType::get(retType, paramTypes, false);
                    vtableFieldTypes.push_back(llvm::PointerType::get(context_, 0));
                    vtable.methodIndices[fd.name] = vtable.methods.size();
                    // 预注册方法函数 / Pre-register method function
                    std::string methodName = cd.name + "." + fd.name;
                    llvm::Function* methodFunc = module_->getFunction(methodName);
                    if (!methodFunc) {
                        methodFunc = llvm::Function::Create(
                            methodTy, llvm::Function::ExternalLinkage, methodName, module_.get());
                        methodFunc->arg_begin()->setName("self");
                        size_t pIdx = 1;
                        for (auto it = std::next(methodFunc->arg_begin());
                             it != methodFunc->arg_end(); ++it) {
                            if (pIdx - 1 < fd.params.size()) {
                                it->setName(fd.params[pIdx - 1].internalName);
                            }
                            pIdx++;
                        }
                        functions_[methodName] = methodFunc;
                    }
                    vtable.methods.push_back(methodFunc);
                }
            }
            // 创建 vtable 类型 / Create vtable type
            if (!vtableFieldTypes.empty()) {
                vtable.vtableType = llvm::StructType::create(context_, cd.name + ".VTable");
                vtable.vtableType->setBody(vtableFieldTypes);

                // 创建全局 vtable 变量并填充方法指针
                // Create global vtable variable and populate with method pointers
                std::vector<llvm::Constant*> vtableEntries;
                for (auto* method : vtable.methods) {
                    // 将函数指针转换为 i8*
                    llvm::Constant* funcPtr = llvm::ConstantExpr::getBitCast(method,
                        llvm::PointerType::get(context_, 0));
                    vtableEntries.push_back(funcPtr);
                }
                llvm::Constant* vtableInit = llvm::ConstantStruct::get(
                    llvm::cast<llvm::StructType>(vtable.vtableType), vtableEntries);
                new llvm::GlobalVariable(*module_, vtable.vtableType, true,
                    llvm::GlobalValue::InternalLinkage, vtableInit, cd.name + ".vtable");
            }
            vtables_[cd.name] = vtable;

            // 只处理 deinit / Only process deinit
            std::string prevDeinitType = currentTypeNameForDeinit_;
            currentTypeNameForDeinit_ = cd.name;
            for (const auto& member : cd.members) {
                if (member && member->declKind == DeclKind::Deinit) {
                    genDecl(*member);
                }
            }
            currentTypeNameForDeinit_ = prevDeinitType;
            currentSuperclassName_ = prevSuperclass;
            break;
        }
        case DeclKind::Actor: {
            // Actor 生成：类似 Class，但添加 executor 序列化
            auto& ad = static_cast<const ActorDecl&>(decl);
            llvm::StructType* actorType = llvm::StructType::create(context_, ad.name);
            std::vector<llvm::Type*> fieldTypes;
            fieldTypes.push_back(llvm::PointerType::get(context_, 0)); // executor
            for (const auto& member : ad.members) {
                if (member && member->declKind == DeclKind::Variable) {
                    auto& vd = static_cast<const VariableDecl&>(*member);
                    fieldTypes.push_back(resolveType(vd.typeAnnotation.get()));
                }
            }
            actorType->setBody(fieldTypes);
            structTypes_[ad.name] = actorType;

            // 注册 Actor 信息 / Register Actor info
            ActorInfo actorInfo;
            actorInfo.type = actorType;
            for (const auto& member : ad.members) {
                if (member && member->declKind == DeclKind::Function) {
                    auto& fd = static_cast<const FunctionDecl&>(*member);
                    std::string methodName = ad.name + "." + fd.name;
                    actorInfo.methods[fd.name] = module_->getFunction(methodName);
                }
            }
            actors_[ad.name] = actorInfo;
            break;
        }
        case DeclKind::Enum: {
            auto& ed = static_cast<const EnumDecl&>(decl);
            // Enum 生成 tagged union 类型
            // 根据 associated values 计算最大 payload 大小
            llvm::Type* payloadType = llvm::Type::getInt64Ty(context_); // 默认 i64
            for (const auto& enumCase : ed.cases) {
                if (enumCase && !enumCase->associatedValues.empty()) {
                    // 使用第一个 associated value 的类型
                    llvm::Type* assocType = resolveType(enumCase->associatedValues[0].type.get());
                    if (assocType && assocType->getPrimitiveSizeInBits() > payloadType->getPrimitiveSizeInBits()) {
                        payloadType = assocType;
                    }
                }
            }
            llvm::StructType* enumType = llvm::StructType::create(context_, ed.name);
            enumType->setBody({llvm::Type::getInt32Ty(context_), payloadType});
            structTypes_[ed.name] = enumType;
            break;
        }
        case DeclKind::If:
            genIfStmt(static_cast<const IfDecl&>(decl));
            break;
        case DeclKind::Guard: {
            // Guard: if condition is false, execute else body (which must exit)
            auto& guard = static_cast<const GuardDecl&>(decl);
            llvm::Value* cond = genExpr(*guard.condition);
            if (!cond) break;
            if (!cond->getType()->isIntegerTy(1)) {
                cond = builder_->CreateICmpNE(cond, llvm::ConstantInt::get(cond->getType(), 0), "tobool");
            }
            llvm::Function* func = builder_->GetInsertBlock()->getParent();
            llvm::BasicBlock* elseBB = llvm::BasicBlock::Create(context_, "guard.else", func);
            llvm::BasicBlock* contBB = llvm::BasicBlock::Create(context_, "guard.cont", func);
            builder_->CreateCondBr(cond, contBB, elseBB);
            builder_->SetInsertPoint(elseBB);
            for (const auto& s : guard.elseBody) { if (s) genStmt(*s); }
            // Guard else body must exit (return/break/continue)
            if (!builder_->GetInsertBlock()->getTerminator()) {
                builder_->CreateBr(contBB); // fallback
            }
            builder_->SetInsertPoint(contBB);
            break;
        }
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
        case DeclKind::Select: {
            // select 语句：channel 多路复用
            // select statement: channel multiplexing with polling
            auto& sd = static_cast<const SelectDecl&>(decl);
            llvm::Function* func = builder_->GetInsertBlock()->getParent();
            llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(context_, "select.end", func);
            llvm::BasicBlock* defaultBB = nullptr;

            // 创建所有 case 块 / Create all case blocks
            std::vector<llvm::BasicBlock*> caseBBs;
            for (size_t i = 0; i < sd.cases.size(); i++) {
                caseBBs.push_back(llvm::BasicBlock::Create(context_, "select.case", func));
            }

            // 查找 default 分支 / Find default branch
            for (size_t i = 0; i < sd.cases.size(); i++) {
                if (sd.cases[i].kind == SelectCase::Kind::Default) {
                    defaultBB = caseBBs[i];
                }
            }
            if (!defaultBB) defaultBB = mergeBB;

            // 创建轮询循环块 / Create polling loop block
            llvm::BasicBlock* pollBB = llvm::BasicBlock::Create(context_, "select.poll", func);
            builder_->CreateBr(pollBB);
            builder_->SetInsertPoint(pollBB);

            // 为每个非 default case 生成 channel 就绪检查
            // Generate channel readiness check for each non-default case
            // 使用条件分支：如果 channel 就绪则跳转到对应 case
            llvm::BasicBlock* nextCheckBB = nullptr;
            for (size_t i = 0; i < sd.cases.size(); i++) {
                if (sd.cases[i].kind == SelectCase::Kind::Default) continue;

                // 生成 channel 表达式 / Generate channel expression
                llvm::Value* channelVal = genExpr(*sd.cases[i].channel);
                if (!channelVal) continue;

                // 生成 channel 就绪检查（简化：检查 channel 指针非空）
                // Generate channel readiness check (simplified: check channel pointer is not null)
                llvm::Value* isReady = builder_->CreateICmpNE(
                    channelVal,
                    llvm::ConstantPointerNull::get(llvm::PointerType::get(context_, 0)),
                    "channel.ready");

                // 创建下一个检查块 / Create next check block
                nextCheckBB = llvm::BasicBlock::Create(context_, "select.next", func);

                // 如果 channel 就绪，跳转到 case；否则继续检查下一个
                builder_->CreateCondBr(isReady, caseBBs[i], nextCheckBB);

                // 设置下一个检查块 / Set next check block
                builder_->SetInsertPoint(nextCheckBB);
            }

            // 所有 channel 都未就绪，跳转到 default 或重新轮询
            // All channels not ready, jump to default or re-poll
            if (defaultBB != mergeBB) {
                // 有 default 分支，执行 default
                builder_->CreateBr(defaultBB);
            } else {
                // 无 default 分支，重新轮询（阻塞等待）
                builder_->CreateBr(pollBB);
            }

            // 生成每个 case 的代码 / Generate code for each case
            for (size_t i = 0; i < sd.cases.size(); i++) {
                builder_->SetInsertPoint(caseBBs[i]);
                for (const auto& s : sd.cases[i].body) {
                    if (s) genStmt(*s);
                }
                if (!builder_->GetInsertBlock()->getTerminator()) {
                    builder_->CreateBr(mergeBB);
                }
            }

            builder_->SetInsertPoint(mergeBB);
            break;
        }
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
        case DeclKind::ExternBlock: {
            // extern 块：处理所有外部声明 / extern block: process all external declarations
            auto& eb = static_cast<const ExternBlockDecl&>(decl);
            for (const auto& d : eb.declarations) {
                if (d) genDecl(*d);
            }
            break;
        }
        case DeclKind::Macro:
            processMacroDecl(static_cast<const MacroDecl&>(decl));
            break;
        case DeclKind::Subscript: {
            auto& sd = static_cast<const SubscriptDecl&>(decl);
            SubscriptInfo info;
            llvm::Type* retType = sd.returnType ? resolveType(sd.returnType.get()) :
                llvm::Type::getInt64Ty(context_);
            info.returnType = retType;

            // 生成 getter 函数 / Generate getter function
            std::vector<llvm::Type*> paramTypes;
            for (const auto& p : sd.params) {
                paramTypes.push_back(p.type ? resolveType(p.type.get()) :
                    llvm::Type::getInt64Ty(context_));
            }
            info.paramTypes = paramTypes;

            if (!sd.getterBody.empty()) {
                std::string getterName = "__subscript_getter_" + std::to_string(nextClosureId_++);
                llvm::FunctionType* getterTy = llvm::FunctionType::get(retType, paramTypes, false);
                llvm::Function* getterFunc = llvm::Function::Create(
                    getterTy, llvm::Function::InternalLinkage, getterName, module_.get());
                llvm::BasicBlock* entry = llvm::BasicBlock::Create(context_, "entry", getterFunc);
                llvm::Function* prevFunc = currentFunc_;
                currentFunc_ = getterFunc;
                builder_->SetInsertPoint(entry);
                auto savedValues = namedValues_;
                auto savedTypes = namedTypes_;
                namedValues_.clear();
                namedTypes_.clear();
                size_t idx = 0;
                for (auto& arg : getterFunc->args()) {
                    if (idx < sd.params.size()) {
                        arg.setName(sd.params[idx].internalName);
                        llvm::AllocaInst* a = createEntryBlockAlloca(getterFunc, arg.getType(),
                            sd.params[idx].internalName);
                        builder_->CreateStore(&arg, a);
                        namedValues_[sd.params[idx].internalName] = a;
                        namedTypes_[sd.params[idx].internalName] = arg.getType();
                    }
                    idx++;
                }
                for (const auto& s : sd.getterBody) {
                    if (s) genStmt(*s);
                }
                if (!builder_->GetInsertBlock()->getTerminator()) {
                    builder_->CreateRet(llvm::Constant::getNullValue(retType));
                }
                namedValues_ = savedValues;
                namedTypes_ = savedTypes;
                currentFunc_ = prevFunc;
                builder_->SetInsertPoint(&currentFunc_->back());
                info.getter = getterFunc;
            }

            // 生成 setter 函数 / Generate setter function
            if (!sd.setterBody.empty()) {
                std::vector<llvm::Type*> setterParamTypes = paramTypes;
                setterParamTypes.push_back(retType); // newValue
                std::string setterName = "__subscript_setter_" + std::to_string(nextClosureId_++);
                llvm::FunctionType* setterTy = llvm::FunctionType::get(
                    llvm::Type::getVoidTy(context_), setterParamTypes, false);
                llvm::Function* setterFunc = llvm::Function::Create(
                    setterTy, llvm::Function::InternalLinkage, setterName, module_.get());
                llvm::BasicBlock* entry = llvm::BasicBlock::Create(context_, "entry", setterFunc);
                llvm::Function* prevFunc = currentFunc_;
                currentFunc_ = setterFunc;
                builder_->SetInsertPoint(entry);
                auto savedValues = namedValues_;
                auto savedTypes = namedTypes_;
                namedValues_.clear();
                namedTypes_.clear();
                size_t idx = 0;
                for (auto& arg : setterFunc->args()) {
                    std::string paramName;
                    if (idx < sd.params.size()) {
                        paramName = sd.params[idx].internalName;
                    } else {
                        paramName = "newValue";
                    }
                    arg.setName(paramName);
                    llvm::AllocaInst* a = createEntryBlockAlloca(setterFunc, arg.getType(), paramName);
                    builder_->CreateStore(&arg, a);
                    namedValues_[paramName] = a;
                    namedTypes_[paramName] = arg.getType();
                    idx++;
                }
                for (const auto& s : sd.setterBody) {
                    if (s) genStmt(*s);
                }
                if (!builder_->GetInsertBlock()->getTerminator()) {
                    builder_->CreateRetVoid();
                }
                namedValues_ = savedValues;
                namedTypes_ = savedTypes;
                currentFunc_ = prevFunc;
                builder_->SetInsertPoint(&currentFunc_->back());
                info.setter = setterFunc;
            }

            subscripts_["__subscript__" + std::to_string(nextClosureId_)] = info;
            break;
        }
        case DeclKind::Init: {
            // 生成 init 函数 / Generate init function
            auto& id = static_cast<const InitDecl&>(decl);
            // init 函数名使用类名.init
            std::string initName = currentTypeNameForDeinit_ + ".init";
            llvm::Function* initFunc = module_->getFunction(initName);
            if (!initFunc) {
                // 创建 init 函数类型：接收 self 指针，返回 void
                std::vector<llvm::Type*> paramTypes;
                paramTypes.push_back(llvm::PointerType::get(context_, 0)); // self
                for (const auto& p : id.params) {
                    paramTypes.push_back(resolveType(p.type.get()));
                }
                llvm::FunctionType* initTy = llvm::FunctionType::get(
                    llvm::Type::getVoidTy(context_), paramTypes, false);
                initFunc = llvm::Function::Create(
                    initTy, llvm::Function::ExternalLinkage, initName, module_.get());
                functions_[initName] = initFunc;
            }
            // 生成 init 函数体（抑制属性观察器）
            if (!id.body.empty()) {
                llvm::BasicBlock* entry = llvm::BasicBlock::Create(context_, "entry", initFunc);
                llvm::Function* prevFunc = currentFunc_;
                bool prevInit = isInInitBody_;
                currentFunc_ = initFunc;
                isInInitBody_ = true;
                builder_->SetInsertPoint(entry);

                // 存储 vtable 指针到对象第一个字段 / Store vtable pointer to object's first field
                if (!currentTypeNameForDeinit_.empty()) {
                    auto vtableIt = vtables_.find(currentTypeNameForDeinit_);
                    if (vtableIt != vtables_.end() && vtableIt->second.vtableType) {
                        llvm::Value* selfPtr = &*initFunc->arg_begin();
                        llvm::Value* vtableGlobal = module_->getGlobalVariable(currentTypeNameForDeinit_ + ".vtable");
                        if (vtableGlobal && selfPtr->getType()->isPointerTy()) {
                            // 使用 i8* 指针进行 GEP
                            llvm::Value* vtableCast = builder_->CreatePointerCast(vtableGlobal,
                                llvm::PointerType::get(context_, 0));
                            builder_->CreateStore(vtableCast, selfPtr);
                        }
                    }
                }

                for (const auto& s : id.body) {
                    if (s) genStmt(*s);
                }
                if (!builder_->GetInsertBlock()->getTerminator()) {
                    builder_->CreateRetVoid();
                }
                currentFunc_ = prevFunc;
                isInInitBody_ = prevInit;
                builder_->SetInsertPoint(&currentFunc_->back());
            }
            break;
        }
        case DeclKind::Deinit: {
            // 生成 deinit 函数 / Generate deinit function
            auto& dd = static_cast<const DeinitDecl&>(decl);
            std::string deinitName = "__deinit_" + std::to_string(nextClosureId_++);
            llvm::FunctionType* deinitTy = llvm::FunctionType::get(
                llvm::Type::getVoidTy(context_), false);
            llvm::Function* deinitFunc = llvm::Function::Create(
                deinitTy, llvm::Function::InternalLinkage, deinitName, module_.get());
            llvm::BasicBlock* entry = llvm::BasicBlock::Create(context_, "entry", deinitFunc);
            llvm::Function* prevFunc = currentFunc_;
            currentFunc_ = deinitFunc;
            builder_->SetInsertPoint(entry);
            for (const auto& s : dd.body) {
                if (s) genStmt(*s);
            }
            if (!builder_->GetInsertBlock()->getTerminator()) {
                builder_->CreateRetVoid();
            }
            currentFunc_ = prevFunc;
            builder_->SetInsertPoint(&currentFunc_->back());
            // 存储 deinit 函数（关联到当前类型）
            if (!currentTypeNameForDeinit_.empty()) {
                deinitFuncs_[currentTypeNameForDeinit_] = deinitFunc;
            }
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

    // 处理属性和调用约定 / Process attributes and calling convention
    for (const auto& attr : decl.attributes) {
        if (attr.name == "_cdecl") {
            func->setCallingConv(llvm::CallingConv::C);
        }
        if (attr.name == "no_mangle") {
            // 已经使用函数名作为链接名，无需额外处理
        }
    }
    // 根据 callingConvention 字段设置调用约定
    if (decl.callingConvention == "C" || decl.callingConvention == "cdecl") {
        func->setCallingConv(llvm::CallingConv::C);
    } else if (decl.callingConvention == "stdcall") {
#ifdef LLVM_CALLINGCONV_X86_STDCALL
        func->setCallingConv(llvm::CallingConv::X86_StdCall);
#else
        func->setCallingConv(llvm::CallingConv::C); // fallback
#endif
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

    // async 函数：生成协程帧 / async function: generate coroutine frame
    if (decl.isAsync) {
        // 创建协程状态变量 / Create coroutine state variable
        llvm::AllocaInst* stateVar = createEntryBlockAlloca(func,
            llvm::Type::getInt32Ty(context_), "__coro_state");
        builder_->CreateStore(llvm::ConstantInt::get(
            llvm::Type::getInt32Ty(context_), 0), stateVar);
        namedValues_["__coro_state"] = stateVar;
        namedTypes_["__coro_state"] = llvm::Type::getInt32Ty(context_);
    }

    // 为参数创建 alloca / Create allocas for parameters
    size_t idx = 0;
    for (auto& arg : func->args()) {
        llvm::Type* paramType = resolveType(decl.params[idx].type.get());
        if (decl.params[idx].isInOut) {
            // inout 参数：直接存储指针 / inout param: store pointer directly
            llvm::AllocaInst* allocaInst = createEntryBlockAlloca(func,
                llvm::PointerType::get(context_, 0), std::string(arg.getName()));
            builder_->CreateStore(&arg, allocaInst);
            namedValues_[std::string(arg.getName())] = allocaInst;
            namedTypes_[std::string(arg.getName())] = llvm::PointerType::get(context_, 0);
        } else {
            llvm::AllocaInst* allocaInst = createEntryBlockAlloca(func, paramType, std::string(arg.getName()));
            builder_->CreateStore(&arg, allocaInst);
            namedValues_[std::string(arg.getName())] = allocaInst;
            namedTypes_[std::string(arg.getName())] = paramType;
        }
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

    // 获取声明类型
    llvm::Type* varType = resolveType(decl.typeAnnotation.get());

    // 计算属性处理：生成 getter/setter 函数
    // Computed property: generate getter/setter functions
    if (!decl.getterBody.empty()) {
        ComputedProp prop;
        prop.valueType = varType;

        // 生成 getter 函数 / Generate getter function
        std::string getterName = varName + ".getter";
        llvm::FunctionType* getterType = llvm::FunctionType::get(varType, false);
        llvm::Function* getterFunc = llvm::Function::Create(
            getterType, llvm::Function::InternalLinkage, getterName, module_.get());
        llvm::BasicBlock* getterEntry = llvm::BasicBlock::Create(context_, "entry", getterFunc);
        llvm::Function* prevFunc = currentFunc_;
        currentFunc_ = getterFunc;
        builder_->SetInsertPoint(getterEntry);
        auto savedValues = namedValues_;
        auto savedTypes = namedTypes_;
        for (const auto& s : decl.getterBody) {
            if (s) genStmt(*s);
        }
        if (!builder_->GetInsertBlock()->getTerminator()) {
            builder_->CreateRet(llvm::Constant::getNullValue(varType));
        }
        namedValues_ = savedValues;
        namedTypes_ = savedTypes;
        currentFunc_ = prevFunc;
        builder_->SetInsertPoint(&currentFunc_->back());
        prop.getter = getterFunc;

        // 生成 setter 函数 / Generate setter function
        if (!decl.setterBody.empty()) {
            std::string setterName = varName + ".setter";
            llvm::FunctionType* setterType = llvm::FunctionType::get(
                llvm::Type::getVoidTy(context_), {varType}, false);
            llvm::Function* setterFunc = llvm::Function::Create(
                setterType, llvm::Function::InternalLinkage, setterName, module_.get());
            // 设置参数名
            setterFunc->arg_begin()->setName("newValue");
            llvm::BasicBlock* setterEntry = llvm::BasicBlock::Create(context_, "entry", setterFunc);
            currentFunc_ = setterFunc;
            builder_->SetInsertPoint(setterEntry);
            savedValues = namedValues_;
            savedTypes = namedTypes_;
            // 注册 newValue 参数
            llvm::AllocaInst* newVar = createEntryBlockAlloca(setterFunc, varType, "newValue");
            builder_->CreateStore(&*setterFunc->arg_begin(), newVar);
            namedValues_["newValue"] = newVar;
            namedTypes_["newValue"] = varType;
            for (const auto& s : decl.setterBody) {
                if (s) genStmt(*s);
            }
            if (!builder_->GetInsertBlock()->getTerminator()) {
                builder_->CreateRetVoid();
            }
            namedValues_ = savedValues;
            namedTypes_ = savedTypes;
            currentFunc_ = prevFunc;
            builder_->SetInsertPoint(&currentFunc_->back());
            prop.setter = setterFunc;
        }

        computedProps_[varName] = prop;
        return; // 计算属性不需要 alloca
    }

    // 属性观察器处理：生成 willSet/didSet 函数
    // Property observers: generate willSet/didSet functions
    if (decl.hasWillSet || decl.hasDidSet) {
        PropertyObserver obs;
        obs.valueType = varType;

        // 生成 willSet 函数 / Generate willSet function
        if (decl.hasWillSet && !decl.willSetBody.empty()) {
            std::string willSetName = varName + ".willSet";
            llvm::FunctionType* willSetType = llvm::FunctionType::get(
                llvm::Type::getVoidTy(context_), {varType}, false);
            llvm::Function* willSetFunc = llvm::Function::Create(
                willSetType, llvm::Function::InternalLinkage, willSetName, module_.get());
            willSetFunc->arg_begin()->setName("newValue");
            llvm::BasicBlock* entry = llvm::BasicBlock::Create(context_, "entry", willSetFunc);
            llvm::Function* prevFunc = currentFunc_;
            currentFunc_ = willSetFunc;
            builder_->SetInsertPoint(entry);
            auto savedValues = namedValues_;
            auto savedTypes = namedTypes_;
            llvm::AllocaInst* newVar = createEntryBlockAlloca(willSetFunc, varType, "newValue");
            builder_->CreateStore(&*willSetFunc->arg_begin(), newVar);
            namedValues_["newValue"] = newVar;
            namedTypes_["newValue"] = varType;
            for (const auto& s : decl.willSetBody) {
                if (s) genStmt(*s);
            }
            if (!builder_->GetInsertBlock()->getTerminator()) {
                builder_->CreateRetVoid();
            }
            namedValues_ = savedValues;
            namedTypes_ = savedTypes;
            currentFunc_ = prevFunc;
            builder_->SetInsertPoint(&currentFunc_->back());
            obs.willSetFunc = willSetFunc;
        }

        // 生成 didSet 函数 / Generate didSet function
        if (decl.hasDidSet && !decl.didSetBody.empty()) {
            std::string didSetName = varName + ".didSet";
            llvm::FunctionType* didSetType = llvm::FunctionType::get(
                llvm::Type::getVoidTy(context_), {varType}, false);
            llvm::Function* didSetFunc = llvm::Function::Create(
                didSetType, llvm::Function::InternalLinkage, didSetName, module_.get());
            didSetFunc->arg_begin()->setName("newValue");
            llvm::BasicBlock* entry = llvm::BasicBlock::Create(context_, "entry", didSetFunc);
            llvm::Function* prevFunc = currentFunc_;
            currentFunc_ = didSetFunc;
            builder_->SetInsertPoint(entry);
            auto savedValues = namedValues_;
            auto savedTypes = namedTypes_;
            llvm::AllocaInst* newVar = createEntryBlockAlloca(didSetFunc, varType, "newValue");
            builder_->CreateStore(&*didSetFunc->arg_begin(), newVar);
            namedValues_["newValue"] = newVar;
            namedTypes_["newValue"] = varType;
            for (const auto& s : decl.didSetBody) {
                if (s) genStmt(*s);
            }
            if (!builder_->GetInsertBlock()->getTerminator()) {
                builder_->CreateRetVoid();
            }
            namedValues_ = savedValues;
            namedTypes_ = savedTypes;
            currentFunc_ = prevFunc;
            builder_->SetInsertPoint(&currentFunc_->back());
            obs.didSetFunc = didSetFunc;
        }

        propertyObservers_[varName] = obs;
    }

    // 生成初始化值
    llvm::Value* initVal = nullptr;
    if (decl.initializer) {
        initVal = genExpr(*decl.initializer);
    }

    // Optional 类型包装：如果声明类型是 Optional 结构体，需要正确包装值
    if (varType->isStructTy() && decl.typeAnnotation &&
        decl.typeAnnotation->typeReprKind == TypeReprKind::Optional) {
        llvm::StructType* optType = llvm::cast<llvm::StructType>(varType);
        if (initVal) {
            // 检查是否是 nil（null 指针）
            bool isNil = initVal->getType()->isPointerTy() &&
                llvm::isa<llvm::ConstantPointerNull>(initVal);
            if (isNil) {
                // nil: 创建零初始化的 Optional / Create zero-initialized Optional
                initVal = llvm::Constant::getNullValue(optType);
            } else {
                // 非 nil: 使用 alloca + GEP + store 构建 / Build with alloca + GEP + store
                llvm::AllocaInst* optAlloca = builder_->CreateAlloca(optType, nullptr, "opt.tmp");
                // 存储值到第一个字段 / Store value to first field
                llvm::Value* valFieldPtr = builder_->CreateStructGEP(optType, optAlloca, 0, "opt.val.ptr");
                llvm::Value* val = initVal;
                llvm::Type* fieldType = optType->getElementType(0);
                if (val->getType() != fieldType) {
                    if (fieldType->isDoubleTy() && val->getType()->isIntegerTy()) {
                        val = builder_->CreateSIToFP(val, fieldType, "opt.cast");
                    } else if (fieldType->isIntegerTy(64) && val->getType()->isIntegerTy()) {
                        val = builder_->CreateSExt(val, fieldType, "opt.cast");
                    }
                }
                builder_->CreateStore(val, valFieldPtr);
                // 存储 hasValue = 1 到第二个字段 / Store hasValue = 1 to second field
                llvm::Value* hasFieldPtr = builder_->CreateStructGEP(optType, optAlloca, 1, "opt.has.ptr");
                builder_->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt8Ty(context_), 1), hasFieldPtr);
                // 加载整个 Optional / Load entire Optional
                initVal = builder_->CreateLoad(optType, optAlloca, "opt.val");
            }
        } else {
            // 无初始化值: 创建零初始化的 Optional
            initVal = llvm::Constant::getNullValue(optType);
        }
    } else if (initVal && initVal->getType() != varType) {
        // 非 Optional 类型的类型转换
        if (varType->isDoubleTy() && initVal->getType()->isIntegerTy()) {
            initVal = builder_->CreateSIToFP(initVal, varType, "cast");
        } else if (varType->isIntegerTy(64) && initVal->getType()->isIntegerTy()) {
            initVal = builder_->CreateSExt(initVal, varType, "cast");
        }
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
        case StmtKind::Return: {
            // 执行 defer 块 / Execute defer blocks
            for (auto it = deferStack_.rbegin(); it != deferStack_.rend(); ++it) {
                for (const auto& deferStmt : *it) {
                    if (deferStmt) genStmt(*deferStmt);
                }
            }
            genReturnStmt(static_cast<const ReturnStmt&>(stmt));
            break;
        }
        case StmtKind::Expression: genExprStmt(static_cast<const ExpressionStmt&>(stmt)); break;
        case StmtKind::Compound: {
            // 进入新的 defer 作用域 / Enter new defer scope
            deferStack_.push_back({});
            genCompoundStmt(static_cast<const CompoundStmt&>(stmt));
            // 退出作用域时执行 defer / Execute defer on scope exit
            auto& defers = deferStack_.back();
            for (auto it = defers.rbegin(); it != defers.rend(); ++it) {
                if (*it) genStmt(**it);
            }
            deferStack_.pop_back();
            break;
        }
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
        case StmtKind::Defer: {
            // 注册 defer 块到当前作用域 / Register defer block to current scope
            if (!deferStack_.empty()) {
                deferStack_.back().push_back(&stmt);
            }
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
    if (!currentFunc_) return;

    // 获取循环变量名 / Get loop variable name
    std::string varName;
    if (decl.pattern && decl.pattern->patternKind == PatternKind::Identifier) {
        varName = static_cast<const IdentifierPattern*>(decl.pattern.get())->name;
    } else {
        return;
    }

    // 生成序列表达式 / Generate sequence expression
    llvm::Value* seqExpr = nullptr;
    if (decl.sequence) {
        seqExpr = genExpr(*decl.sequence);
    }
    if (!seqExpr) return;

    // 获取序列长度 / Get sequence length
    // 如果序列是 Array 类型 {i8*, i64, i64}，第二个元素是 count
    llvm::Value* length = nullptr;
    llvm::Type* elemType = llvm::Type::getInt64Ty(context_);
    if (seqExpr->getType()->isStructTy()) {
        // Array 类型: { data_ptr, count, capacity }
        llvm::Value* countPtr = builder_->CreateStructGEP(seqExpr->getType(), seqExpr, 1, "arr.count.ptr");
        length = builder_->CreateLoad(llvm::Type::getInt64Ty(context_), countPtr, "arr.count");
        // 数据指针
        llvm::Value* dataPtr = builder_->CreateStructGEP(seqExpr->getType(), seqExpr, 0, "arr.data.ptr");
        llvm::Value* data = builder_->CreateLoad(llvm::PointerType::get(context_, 0), dataPtr, "arr.data");
        // 存储数据指针以便后续按索引访问
        seqExpr = data;
    } else if (seqExpr->getType()->isPointerTy()) {
        // 指针类型：尝试从数组结构加载长度
        // Pointer type: try to load length from array struct
        // 如果指针指向 {data_ptr, count, capacity} 结构
        llvm::Type* i64Ty = llvm::Type::getInt64Ty(context_);
        llvm::Value* countPtr = builder_->CreateGEP(i64Ty, seqExpr,
            llvm::ConstantInt::get(i64Ty, 1), "arr.count.ptr");
        length = builder_->CreateLoad(i64Ty, countPtr, "arr.count");
    } else {
        // 其他类型：默认空迭代
        length = llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), 0);
    }

    // 创建循环变量 / Create loop variable
    llvm::AllocaInst* loopVar = createEntryBlockAlloca(currentFunc_, elemType, varName);

    // 创建索引变量 / Create index variable
    llvm::AllocaInst* indexVar = createEntryBlockAlloca(currentFunc_, llvm::Type::getInt64Ty(context_), "__for_idx");
    builder_->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), 0), indexVar);

    // 创建基本块 / Create basic blocks
    llvm::Function* func = builder_->GetInsertBlock()->getParent();
    llvm::BasicBlock* condBB = llvm::BasicBlock::Create(context_, "for.cond", func);
    llvm::BasicBlock* bodyBB = llvm::BasicBlock::Create(context_, "for.body", func);
    llvm::BasicBlock* endBB = llvm::BasicBlock::Create(context_, "for.end", func);

    loopStack_.push_back({condBB, endBB});

    // 跳转到条件块 / Jump to condition block
    builder_->CreateBr(condBB);

    // 条件块: index < length / Condition block: index < length
    builder_->SetInsertPoint(condBB);
    llvm::Value* index = builder_->CreateLoad(llvm::Type::getInt64Ty(context_), indexVar, "idx");
    llvm::Value* cond = builder_->CreateICmpSLT(index, length, "for.cond");
    builder_->CreateCondBr(cond, bodyBB, endBB);

    // 循环体 / Loop body
    builder_->SetInsertPoint(bodyBB);

    // 从数组中加载元素 / Load element from array
    if (seqExpr && seqExpr->getType()->isPointerTy()) {
        llvm::Value* elemPtr = builder_->CreateGEP(elemType, seqExpr, index, "elem.ptr");
        llvm::Value* elem = builder_->CreateLoad(elemType, elemPtr, "elem");
        builder_->CreateStore(elem, loopVar);
    } else {
        builder_->CreateStore(index, loopVar);
    }
    namedValues_[varName] = loopVar;
    namedTypes_[varName] = elemType;

    // 生成循环体 / Generate loop body
    for (const auto& s : decl.body) {
        if (s) genStmt(*s);
    }

    // 递增索引 / Increment index
    if (!builder_->GetInsertBlock()->getTerminator()) {
        llvm::Value* newIndex = builder_->CreateAdd(index,
            llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), 1), "idx.inc");
        builder_->CreateStore(newIndex, indexVar);
        builder_->CreateBr(condBB);
    }

    // 结束块 / End block
    builder_->SetInsertPoint(endBB);
    loopStack_.pop_back();
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
    // 对于枚举类型，提取 tag 进行比较
    // For enum types, extract tag for comparison
    llvm::Value* tagValue = nullptr;
    if (subject->getType()->isStructTy()) {
        // 枚举类型：提取第一个字段（tag）
        llvm::Value* tagPtr = builder_->CreateStructGEP(
            subject->getType(), subject, 0, "enum.tag.ptr");
        tagValue = builder_->CreateLoad(llvm::Type::getInt32Ty(context_), tagPtr, "enum.tag");
    }

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
                // 使用 tag 值进行比较（如果有）
                llvm::Value* compareVal = tagValue ? tagValue : subject;
                llvm::Value* cmp = builder_->CreateICmpEQ(compareVal, caseVal, "casecmp");
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
    llvm::Function* func = builder_->GetInsertBlock()->getParent();

    // 声明 setjmp / Declare setjmp
    llvm::Function* setjmpFunc = module_->getFunction("setjmp");
    if (!setjmpFunc) {
        llvm::Type* ptrTy = llvm::PointerType::get(context_, 0);
        llvm::FunctionType* setjmpTy = llvm::FunctionType::get(
            llvm::Type::getInt32Ty(context_), {ptrTy}, false);
        setjmpFunc = llvm::Function::Create(setjmpTy, llvm::Function::ExternalLinkage,
                                            "setjmp", module_.get());
    }

    // 分配 jmp_buf / Allocate jmp_buf (256 bytes should be enough)
    llvm::AllocaInst* jmpBuf = builder_->CreateAlloca(
        llvm::Type::getInt8Ty(context_),
        llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), 256), "jmpbuf");

    // 创建基本块 / Create basic blocks
    llvm::BasicBlock* doBB = llvm::BasicBlock::Create(context_, "do.body", func);
    llvm::BasicBlock* catchBB = llvm::BasicBlock::Create(context_, "catch.body", func);
    llvm::BasicBlock* endBB = llvm::BasicBlock::Create(context_, "do.end", func);

    // 调用 setjmp / Call setjmp
    llvm::Value* jmpResult = builder_->CreateCall(setjmpFunc, {jmpBuf}, "setjmp.result");
    llvm::Value* isThrow = builder_->CreateICmpNE(jmpResult,
        llvm::ConstantInt::get(llvm::Type::getInt32Ty(context_), 0), "is.throw");

    // 如果 setjmp 返回 0，正常执行 do 块；否则跳转到 catch
    // If setjmp returns 0, execute do block normally; otherwise jump to catch
    builder_->CreateCondBr(isThrow, catchBB, doBB);

    // Do 块 / Do block
    builder_->SetInsertPoint(doBB);

    // 将 jmp_buf 压入 catch 栈 / Push jmp_buf to catch stack
    catchStack_.push_back(jmpBuf);

    // 生成 do 块体 / Generate do block body
    for (const auto& s : decl.doBody) {
        if (s) genStmt(*s);
    }

    // 弹出 catch 栈 / Pop catch stack
    if (!catchStack_.empty()) catchStack_.pop_back();

    // 如果 do 块没有终结指令，跳转到结束块
    if (!builder_->GetInsertBlock()->getTerminator()) {
        builder_->CreateBr(endBB);
    }

    // Catch 块 / Catch block
    builder_->SetInsertPoint(catchBB);
    for (const auto& catchClause : decl.catches) {
        for (const auto& s : catchClause.body) {
            if (s) genStmt(*s);
        }
    }
    if (!builder_->GetInsertBlock()->getTerminator()) {
        builder_->CreateBr(endBB);
    }

    // 结束块 / End block
    builder_->SetInsertPoint(endBB);
}

void IRGenerator::genThrowStmt(const ThrowDecl& decl) {
    // 使用 longjmp 进行异常跳转 / Use longjmp for exception jumping
    // 如果有 catch 块，跳转到 catch 块；否则直接返回
    // If there is a catch block, jump to it; otherwise return directly

    // 生成错误值 / Generate error value
    llvm::Value* errorVal = nullptr;
    if (decl.value) {
        errorVal = genExpr(*decl.value);
    }

    // 如果有活跃的 catch 块，使用 longjmp 跳转
    // If there is an active catch block, use longjmp to jump
    if (!catchStack_.empty()) {
        // 调用 longjmp 跳转到 catch 块
        llvm::Function* longjmpFunc = module_->getFunction("longjmp");
        if (!longjmpFunc) {
            llvm::Type* ptrTy = llvm::PointerType::get(context_, 0);
            llvm::FunctionType* longjmpTy = llvm::FunctionType::get(
                llvm::Type::getVoidTy(context_), {ptrTy, llvm::Type::getInt32Ty(context_)}, false);
            longjmpFunc = llvm::Function::Create(longjmpTy, llvm::Function::ExternalLinkage,
                                                 "longjmp", module_.get());
        }
        builder_->CreateCall(longjmpFunc, {catchStack_.back(),
            llvm::ConstantInt::get(llvm::Type::getInt32Ty(context_), 1)});
    } else {
        // 没有 catch 块，直接返回 / No catch block, return directly
        if (currentFunc_->getReturnType()->isVoidTy()) {
            builder_->CreateRetVoid();
        } else {
            builder_->CreateRet(llvm::Constant::getNullValue(currentFunc_->getReturnType()));
        }
    }
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
        case ExprKind::SetLiteral:
            return genSetLiteral(static_cast<const SetLiteralExpr&>(expr));
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
        case ExprKind::TypeCheck: {
            // is 类型检查：RTTI 类型标签比较
            auto& tc = static_cast<const TypeCheckExpr&>(expr);
            llvm::Value* val = genExpr(*tc.subExpr);
            if (!val) return nullptr;

            if (!tc.checkType) return llvm::ConstantInt::getTrue(context_);

            // 解析目标类型名
            std::string targetTypeName;
            if (tc.checkType->typeReprKind == TypeReprKind::Named) {
                targetTypeName = static_cast<const NamedTypeRepr&>(*tc.checkType).name;
            }

            // 对于值类型，比较 LLVM 类型
            if (!val->getType()->isPointerTy()) {
                llvm::Type* targetType = getLLVMType(targetTypeName);
                if (val->getType() == targetType) {
                    return llvm::ConstantInt::getTrue(context_);
                }
                return llvm::ConstantInt::getFalse(context_);
            }

            // 对于引用类型（指针），检查是否非 null
            // 完整 RTTI 需要类型标签比较
            llvm::Value* isNonNull = builder_->CreateICmpNE(val,
                llvm::ConstantPointerNull::get(llvm::PointerType::get(context_, 0)), "is.nonnull");

            // 如果目标类型是已知的结构体/类，生成类型 ID 比较
            auto structIt = structTypes_.find(targetTypeName);
            if (structIt != structTypes_.end()) {
                // 生成类型 ID 常量
                llvm::GlobalVariable* typeIdGlobal = module_->getGlobalVariable(targetTypeName + ".typeid");
                if (!typeIdGlobal) {
                    llvm::Constant* typeId = llvm::ConstantInt::get(
                        llvm::Type::getInt64Ty(context_), std::hash<std::string>{}(targetTypeName));
                    typeIdGlobal = new llvm::GlobalVariable(*module_,
                        llvm::Type::getInt64Ty(context_), true,
                        llvm::GlobalValue::InternalLinkage, typeId, targetTypeName + ".typeid");
                }
                // 从对象加载类型 ID（假设在 vtable 指针之后）
                llvm::Value* typeIdPtr = builder_->CreateGEP(
                    llvm::Type::getInt8Ty(context_), val,
                    llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), sizeof(void*)),
                    "typeid.ptr");
                llvm::Value* typeIdCast = builder_->CreatePointerCast(typeIdPtr,
                    llvm::PointerType::get(context_, 0));
                llvm::Value* loadedTypeId = builder_->CreateLoad(
                    llvm::Type::getInt64Ty(context_), typeIdCast, "typeid.loaded");
                llvm::Value* expectedTypeId = builder_->CreateLoad(
                    llvm::Type::getInt64Ty(context_), typeIdGlobal, "typeid.expected");
                llvm::Value* typeMatch = builder_->CreateICmpEQ(loadedTypeId, expectedTypeId, "typeid.match");
                return builder_->CreateAnd(isNonNull, typeMatch, "is.check");
            }

            return isNonNull;
        }
        case ExprKind::Await: {
            auto& ae = static_cast<const AwaitExpr&>(expr);
            llvm::Value* val = genExpr(*ae.subExpr);
            if (!val) return nullptr;
            // async 上下文中：创建挂起点 / In async context: create suspension point
            if (isInAsyncFunc_) {
                // 递增状态 / Increment state
                llvm::Value* stateVar = namedValues_["__coro_state"];
                if (stateVar) {
                    llvm::Value* currentState = builder_->CreateLoad(
                        llvm::Type::getInt32Ty(context_), stateVar, "coro.state");
                    llvm::Value* nextState = builder_->CreateAdd(currentState,
                        llvm::ConstantInt::get(llvm::Type::getInt32Ty(context_), 1), "coro.next");
                    builder_->CreateStore(nextState, stateVar);
                    awaitPointCount_++;
                }
            }
            return val;
        }
        case ExprKind::Try: {
            auto& te = static_cast<const TryExpr&>(expr);
            if (te.isOptional) {
                // try? expr: 使用 setjmp/longjmp 捕获错误返回 nil
                // Use setjmp/longjmp to catch errors and return nil
                llvm::Function* setjmpFunc = module_->getFunction("setjmp");
                if (!setjmpFunc) {
                    llvm::Type* ptrTy = llvm::PointerType::get(context_, 0);
                    llvm::FunctionType* setjmpTy = llvm::FunctionType::get(
                        llvm::Type::getInt32Ty(context_), {ptrTy}, false);
                    setjmpFunc = llvm::Function::Create(setjmpTy, llvm::Function::ExternalLinkage,
                                                        "setjmp", module_.get());
                }
                llvm::AllocaInst* jmpBuf = builder_->CreateAlloca(
                    llvm::Type::getInt8Ty(context_),
                    llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), 256), "try.jmpbuf");
                llvm::Value* jmpResult = builder_->CreateCall(setjmpFunc, {jmpBuf}, "try.setjmp");
                llvm::Value* isThrow = builder_->CreateICmpNE(jmpResult,
                    llvm::ConstantInt::get(llvm::Type::getInt32Ty(context_), 0), "try.is_throw");

                llvm::Function* func = builder_->GetInsertBlock()->getParent();
                llvm::BasicBlock* tryBB = llvm::BasicBlock::Create(context_, "try.body", func);
                llvm::BasicBlock* catchBB = llvm::BasicBlock::Create(context_, "try.catch", func);
                llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(context_, "try.merge", func);

                builder_->CreateCondBr(isThrow, catchBB, tryBB);

                // Try 块 / Try block
                builder_->SetInsertPoint(tryBB);
                catchStack_.push_back(jmpBuf);
                llvm::Value* val = genExpr(*te.subExpr);
                catchStack_.pop_back();
                if (val) {
                    builder_->CreateBr(mergeBB);
                }
                llvm::BasicBlock* tryEndBB = builder_->GetInsertBlock();

                // Catch 块：返回 nil / Catch block: return nil
                builder_->SetInsertPoint(catchBB);
                llvm::Value* nilVal = val && val->getType()->isPointerTy() ?
                    llvm::ConstantPointerNull::get(llvm::PointerType::get(context_, 0)) :
                    llvm::Constant::getNullValue(val ? val->getType() : llvm::Type::getInt64Ty(context_));
                builder_->CreateBr(mergeBB);
                llvm::BasicBlock* catchEndBB = builder_->GetInsertBlock();

                // Merge 块 / Merge block
                builder_->SetInsertPoint(mergeBB);
                llvm::Type* resultType = val ? val->getType() : llvm::Type::getInt64Ty(context_);
                llvm::PHINode* phi = builder_->CreatePHI(resultType, 2, "try.result");
                if (val) phi->addIncoming(val, tryEndBB);
                phi->addIncoming(nilVal, catchEndBB);
                return phi;
            } else if (te.isForce) {
                // try! expr: 使用 setjmp/longjmp 捕获错误调用 abort
                llvm::Function* setjmpFunc = module_->getFunction("setjmp");
                if (!setjmpFunc) {
                    llvm::Type* ptrTy = llvm::PointerType::get(context_, 0);
                    llvm::FunctionType* setjmpTy = llvm::FunctionType::get(
                        llvm::Type::getInt32Ty(context_), {ptrTy}, false);
                    setjmpFunc = llvm::Function::Create(setjmpTy, llvm::Function::ExternalLinkage,
                                                        "setjmp", module_.get());
                }
                llvm::AllocaInst* jmpBuf = builder_->CreateAlloca(
                    llvm::Type::getInt8Ty(context_),
                    llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), 256), "try.jmpbuf");
                llvm::Value* jmpResult = builder_->CreateCall(setjmpFunc, {jmpBuf}, "try.setjmp");
                llvm::Value* isThrow = builder_->CreateICmpNE(jmpResult,
                    llvm::ConstantInt::get(llvm::Type::getInt32Ty(context_), 0), "try.is_throw");

                llvm::Function* func = builder_->GetInsertBlock()->getParent();
                llvm::BasicBlock* tryBB = llvm::BasicBlock::Create(context_, "try.body", func);
                llvm::BasicBlock* catchBB = llvm::BasicBlock::Create(context_, "try.catch", func);

                builder_->CreateCondBr(isThrow, catchBB, tryBB);

                // Try 块 / Try block
                builder_->SetInsertPoint(tryBB);
                catchStack_.push_back(jmpBuf);
                llvm::Value* val = genExpr(*te.subExpr);
                catchStack_.pop_back();
                return val;
            } else {
                // 普通 try / Regular try
                return genExpr(*te.subExpr);
            }
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
        case ExprKind::SelfRef: {
            // self 引用当前实例 / self references current instance
            auto it = namedValues_.find("self");
            if (it != namedValues_.end()) {
                llvm::Type* ty = namedTypes_["self"];
                return builder_->CreateLoad(ty, it->second, "self");
            }
            return nullptr;
        }
        case ExprKind::SuperRef: {
            // super 引用父类 - 返回 self 指针，但标记为 super 调用
            // super references parent class - return self pointer, mark as super call
            auto it = namedValues_.find("self");
            if (it != namedValues_.end()) {
                llvm::Type* ty = namedTypes_["self"];
                isSuperCall_ = true;
                return builder_->CreateLoad(ty, it->second, "super");
            }
            return nullptr;
        }
        case ExprKind::ForceUnwrap: {
            // 强制解包：如果值为 null 则调用 abort
            auto& fu = static_cast<const ForceUnwrapExpr&>(expr);
            llvm::Value* val = genExpr(*fu.subExpr);
            if (!val) return nullptr;
            // 对于指针类型，检查是否为 null
            if (val->getType()->isPointerTy()) {
                llvm::Function* func = builder_->GetInsertBlock()->getParent();
                llvm::BasicBlock* checkBB = llvm::BasicBlock::Create(context_, "unwrap.check", func);
                llvm::BasicBlock* contBB = llvm::BasicBlock::Create(context_, "unwrap.ok", func);

                llvm::Value* isNull = builder_->CreateICmpEQ(val,
                    llvm::ConstantPointerNull::get(llvm::PointerType::get(context_, 0)), "is.null");
                builder_->CreateCondBr(isNull, checkBB, contBB);

                // null 分支：调用 abort
                builder_->SetInsertPoint(checkBB);
                llvm::Function* abortFunc = module_->getFunction("abort");
                if (!abortFunc) {
                    llvm::FunctionType* abortTy = llvm::FunctionType::get(
                        llvm::Type::getVoidTy(context_), false);
                    abortFunc = llvm::Function::Create(abortTy, llvm::Function::ExternalLinkage,
                                                       "abort", module_.get());
                }
                builder_->CreateCall(abortFunc);
                builder_->CreateUnreachable();

                // 非 null 分支
                builder_->SetInsertPoint(contBB);
            }
            return val;
        }
        case ExprKind::OptionalChain: {
            // 可选链：如果值为 null，整个表达式返回 null
            auto& oc = static_cast<const OptionalChainExpr&>(expr);
            llvm::Value* val = genExpr(*oc.subExpr);
            if (!val) return nullptr;
            // 对于指针类型，如果为 null 则短路返回 null
            if (val->getType()->isPointerTy()) {
                llvm::Function* func = builder_->GetInsertBlock()->getParent();
                llvm::BasicBlock* nonNullBB = llvm::BasicBlock::Create(context_, "chain.nonnull", func);
                llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(context_, "chain.merge", func);

                llvm::Value* isNull = builder_->CreateICmpEQ(val,
                    llvm::ConstantPointerNull::get(llvm::PointerType::get(context_, 0)), "is.null");
                builder_->CreateCondBr(isNull, mergeBB, nonNullBB);

                // 非 null 分支：继续执行
                builder_->SetInsertPoint(nonNullBB);
                builder_->CreateBr(mergeBB);
                nonNullBB = builder_->GetInsertBlock();

                // merge 分支：PHI 选择结果
                builder_->SetInsertPoint(mergeBB);
                llvm::PHINode* phi = builder_->CreatePHI(val->getType(), 2, "chain.result");
                phi->addIncoming(val, nonNullBB);
                phi->addIncoming(llvm::ConstantPointerNull::get(llvm::PointerType::get(context_, 0)),
                    &func->getEntryBlock());
                return phi;
            }
            return val;
        }
        case ExprKind::Subscript: {
            auto& sub = static_cast<const SubscriptExpr&>(expr);
            llvm::Value* base = genExpr(*sub.base);
            if (!base || sub.indices.empty()) return nullptr;

            // 生成索引参数 / Generate index arguments
            std::vector<llvm::Value*> indices;
            for (const auto& idx : sub.indices) {
                llvm::Value* idxVal = genExpr(*idx);
                if (idxVal) indices.push_back(idxVal);
            }
            if (indices.empty()) return nullptr;

            // 检查是否有注册的 subscript getter / Check for registered subscript getter
            for (const auto& [name, info] : subscripts_) {
                if (info.getter && indices.size() == info.paramTypes.size()) {
                    // 类型转换 / Type conversion
                    std::vector<llvm::Value*> args;
                    for (size_t i = 0; i < indices.size(); i++) {
                        llvm::Value* arg = indices[i];
                        if (i < info.paramTypes.size() && arg->getType() != info.paramTypes[i]) {
                            if (info.paramTypes[i]->isIntegerTy(64) && arg->getType()->isIntegerTy()) {
                                arg = builder_->CreateSExt(arg, info.paramTypes[i], "cast");
                            }
                        }
                        args.push_back(arg);
                    }
                    return builder_->CreateCall(info.getter, args, "subscript.get");
                }
            }

            // 默认：数组 GEP / Default: array GEP
            llvm::Value* index = indices[0];
            llvm::Type* elemType = llvm::Type::getInt64Ty(context_);
            if (base->getType()->isStructTy()) {
                llvm::Value* dataPtr = builder_->CreateStructGEP(
                    base->getType(), base, 0, "arr.data.ptr");
                llvm::Value* data = builder_->CreateLoad(
                    llvm::PointerType::get(context_, 0), dataPtr, "arr.data");
                return builder_->CreateGEP(elemType, data, index, "idx");
            } else if (base->getType()->isPointerTy()) {
                return builder_->CreateGEP(elemType, base, index, "idx");
            }
            return builder_->CreateGEP(elemType, base, index, "idx");
        }
        case ExprKind::Closure: {
            // 闭包实现：捕获外部变量作为额外参数
            // Closure: capture outer variables as additional parameters
            auto& closure = static_cast<const ClosureExpr&>(expr);
            std::string funcName = "__closure_" + std::to_string(nextClosureId_++);

            // 解析捕获列表 / Parse capture list
            // 格式: "weak self", "unowned delegate", "strong varName"
            std::unordered_map<std::string, std::string> captureSemantics; // name -> "weak"/"unowned"/"strong"
            for (const auto& capture : closure.captureList) {
                std::string name = capture;
                std::string semantic = "strong"; // 默认强引用
                if (name.substr(0, 5) == "weak ") {
                    semantic = "weak";
                    name = name.substr(5);
                } else if (name.substr(0, 8) == "unowned ") {
                    semantic = "unowned";
                    name = name.substr(8);
                }
                captureSemantics[name] = semantic;
            }

            // 收集需要捕获的变量（当前作用域中的所有变量）
            // Collect variables to capture (all variables in current scope)
            std::vector<std::string> capturedNames;
            std::vector<llvm::Type*> capturedTypes;
            std::vector<llvm::Value*> capturedValues;
            std::vector<bool> capturedIsRef;
            std::vector<std::string> capturedSemantic; // "weak"/"unowned"/"strong"
            for (const auto& [name, allocaInst] : namedValues_) {
                // 跳过参数（它们会在闭包参数中）
                bool isParam = false;
                for (const auto& p : closure.params) {
                    if (p.name == name) { isParam = true; break; }
                }
                if (isParam) continue;
                capturedNames.push_back(name);
                capturedTypes.push_back(namedTypes_[name]);
                capturedValues.push_back(allocaInst);
                capturedIsRef.push_back(isReferenceType(namedTypes_[name]));

                // 确定捕获语义 / Determine capture semantics
                auto capIt = captureSemantics.find(name);
                std::string semantic = (capIt != captureSemantics.end()) ? capIt->second : "strong";
                capturedSemantic.push_back(semantic);

                // ARC: 根据捕获语义处理引用类型
                if (isReferenceType(namedTypes_[name])) {
                    llvm::Value* val = builder_->CreateLoad(namedTypes_[name], allocaInst, name + ".capture");
                    if (semantic == "strong") {
                        insertRetain(val);
                    }
                    // weak 和 unowned 不需要 retain
                }
            }

            // 创建函数类型：显式参数 + 捕获变量参数
            std::vector<llvm::Type*> paramTypes;
            for (const auto& p : closure.params) {
                paramTypes.push_back(llvm::Type::getInt64Ty(context_));
            }
            // 添加捕获变量的指针参数
            for (auto* ty : capturedTypes) {
                paramTypes.push_back(llvm::PointerType::get(context_, 0));
            }
            // 确定闭包返回类型 / Determine closure return type
            llvm::Type* retType = closure.returnType ?
                resolveType(closure.returnType.get()) :
                llvm::Type::getVoidTy(context_);
            llvm::FunctionType* funcType = llvm::FunctionType::get(retType, paramTypes, false);
            llvm::Function* func = llvm::Function::Create(
                funcType, llvm::Function::InternalLinkage, funcName, module_.get());

            // 生成函数体
            llvm::BasicBlock* entry = llvm::BasicBlock::Create(context_, "entry", func);
            builder_->SetInsertPoint(entry);

            // 保存当前上下文
            auto savedValues = namedValues_;
            auto savedTypes = namedTypes_;
            llvm::Function* savedFunc = currentFunc_;
            currentFunc_ = func;

            // 设置参数
            namedValues_.clear();
            namedTypes_.clear();
            size_t idx = 0;
            for (auto& arg : func->args()) {
                if (idx < closure.params.size()) {
                    // 显式参数
                    arg.setName(closure.params[idx].name);
                    llvm::AllocaInst* allocaInst = createEntryBlockAlloca(func, arg.getType(), closure.params[idx].name);
                    builder_->CreateStore(&arg, allocaInst);
                    namedValues_[closure.params[idx].name] = allocaInst;
                    namedTypes_[closure.params[idx].name] = arg.getType();
                } else {
                    // 捕获的变量：从指针加载
                    size_t capIdx = idx - closure.params.size();
                    if (capIdx < capturedNames.size()) {
                        std::string capName = capturedNames[capIdx];
                        llvm::Type* capType = capturedTypes[capIdx];
                        arg.setName(capName + ".cap");
                        // 创建本地 alloca 并从捕获指针加载值
                        llvm::AllocaInst* allocaInst = createEntryBlockAlloca(func, capType, capName);
                        llvm::Value* loaded = builder_->CreateLoad(capType, &arg, capName + ".loaded");
                        builder_->CreateStore(loaded, allocaInst);
                        namedValues_[capName] = allocaInst;
                        namedTypes_[capName] = capType;
                    }
                }
                idx++;
            }

            // 生成闭包体
            for (const auto& stmt : closure.body) {
                if (stmt) genStmt(*stmt);
            }

            // 添加返回
            if (!builder_->GetInsertBlock()->getTerminator()) {
                builder_->CreateRetVoid();
            }

            // 恢复上下文
            currentFunc_ = savedFunc;
            namedValues_ = savedValues;
            namedTypes_ = savedTypes;
            builder_->SetInsertPoint(&currentFunc_->back());

            // 返回闭包函数指针
            return func;
        }
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
                // 检查计算属性 setter / Check computed property setter
                auto compIt = computedProps_.find(id.name);
                if (compIt != computedProps_.end() && compIt->second.setter) {
                    if (val->getType() != compIt->second.valueType) {
                        if (compIt->second.valueType->isDoubleTy() && val->getType()->isIntegerTy()) {
                            val = builder_->CreateSIToFP(val, compIt->second.valueType, "cast");
                        }
                    }
                    builder_->CreateCall(compIt->second.setter, {val});
                    return val;
                }
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
                            // 属性观察器：willSet（赋值前调用，init 中不调用）
                            if (!isInInitBody_) {
                                auto obsIt2 = propertyObservers_.find(id.name);
                                if (obsIt2 != propertyObservers_.end() && obsIt2->second.willSetFunc) {
                                    builder_->CreateCall(obsIt2->second.willSetFunc, {result});
                                }
                            }
                            builder_->CreateStore(result, it->second);
                            // 属性观察器：didSet（赋值后调用，init 中不调用）
                            if (!isInInitBody_) {
                                auto obsIt2 = propertyObservers_.find(id.name);
                                if (obsIt2 != propertyObservers_.end() && obsIt2->second.didSetFunc) {
                                    builder_->CreateCall(obsIt2->second.didSetFunc, {result});
                                }
                            }
                            return result;
                        }
                    }

                    // 简单赋值 / Simple assignment
                    if (val->getType() != varType) {
                        if (varType->isDoubleTy() && val->getType()->isIntegerTy()) {
                            val = builder_->CreateSIToFP(val, varType, "cast");
                        }
                    }
                    // 属性观察器：willSet（赋值前调用，init 中不调用）
                    if (!isInInitBody_) {
                        auto obsIt = propertyObservers_.find(id.name);
                        if (obsIt != propertyObservers_.end() && obsIt->second.willSetFunc) {
                            builder_->CreateCall(obsIt->second.willSetFunc, {val});
                        }
                    }
                    builder_->CreateStore(val, it->second);
                    // 属性观察器：didSet（赋值后调用，init 中不调用）
                    if (!isInInitBody_) {
                        auto obsIt = propertyObservers_.find(id.name);
                        if (obsIt != propertyObservers_.end() && obsIt->second.didSetFunc) {
                            builder_->CreateCall(obsIt->second.didSetFunc, {val});
                        }
                    }
                    return val;
                }
            }
            return val;
        }
        case ExprKind::MacroExpansion:
            return genMacroExpansion(static_cast<const MacroExpansionExpr&>(expr));
        case ExprKind::Selector: {
            // #selector(method) - 返回方法名字符串
            auto& sel = static_cast<const SelectorExpr&>(expr);
            if (sel.method && sel.method->exprKind == ExprKind::Identifier) {
                auto& id = static_cast<const IdentifierExpr&>(*sel.method);
                return createStringGlobal(id.name);
            }
            return createStringGlobal("");
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
    // 检查计算属性 / Check computed properties
    auto compIt = computedProps_.find(expr.name);
    if (compIt != computedProps_.end() && compIt->second.getter) {
        return builder_->CreateCall(compIt->second.getter, {}, expr.name + ".get");
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
        case TokenKind::QuestionQuestion: {
            // ?? nil 合并运算符 / Nil coalescing operator
            // 如果左侧非 nil 则返回左侧，否则返回右侧
            if (left->getType()->isPointerTy()) {
                llvm::Value* isNull = builder_->CreateICmpEQ(left,
                    llvm::ConstantPointerNull::get(llvm::PointerType::get(context_, 0)), "is.null");
                llvm::Function* func = builder_->GetInsertBlock()->getParent();
                llvm::BasicBlock* nonNullBB = llvm::BasicBlock::Create(context_, "coalesce.nonnull", func);
                llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(context_, "coalesce.merge", func);
                builder_->CreateCondBr(isNull, mergeBB, nonNullBB);
                llvm::BasicBlock* leftBB = builder_->GetInsertBlock();
                builder_->SetInsertPoint(nonNullBB);
                builder_->CreateBr(mergeBB);
                nonNullBB = builder_->GetInsertBlock();
                builder_->SetInsertPoint(mergeBB);
                llvm::PHINode* phi = builder_->CreatePHI(left->getType(), 2, "coalesce");
                phi->addIncoming(left, leftBB);
                phi->addIncoming(right, nonNullBB);
                return phi;
            }
            // 对于 Optional 结构体类型
            if (left->getType()->isStructTy()) {
                // 检查 hasValue 字段（索引 1）
                llvm::Value* hasValuePtr = builder_->CreateStructGEP(
                    left->getType(), left, 1, "opt.hasvalue.ptr");
                llvm::Value* hasValue = builder_->CreateLoad(
                    llvm::Type::getInt1Ty(context_), hasValuePtr, "opt.hasvalue");
                llvm::Function* func = builder_->GetInsertBlock()->getParent();
                llvm::BasicBlock* hasValueBB = llvm::BasicBlock::Create(context_, "coalesce.hasvalue", func);
                llvm::BasicBlock* mergeBB = llvm::BasicBlock::Create(context_, "coalesce.merge", func);
                builder_->CreateCondBr(hasValue, hasValueBB, mergeBB);
                llvm::BasicBlock* leftBB = builder_->GetInsertBlock();
                builder_->SetInsertPoint(hasValueBB);
                builder_->CreateBr(mergeBB);
                hasValueBB = builder_->GetInsertBlock();
                builder_->SetInsertPoint(mergeBB);
                llvm::PHINode* phi = builder_->CreatePHI(left->getType(), 2, "coalesce");
                phi->addIncoming(left, leftBB);
                phi->addIncoming(right, hasValueBB);
                return phi;
            }
            // 默认：如果左侧非零返回左侧
            llvm::Value* isZero = builder_->CreateICmpEQ(left,
                llvm::ConstantInt::get(left->getType(), 0), "is.zero");
            return builder_->CreateSelect(isZero, right, left, "coalesce");
        }
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
    // 处理成员方法调用（通过 vtable 调度）/ Handle member method calls (via vtable dispatch)
    if (expr.callee->exprKind == ExprKind::MemberAccess) {
        auto& ma = static_cast<const MemberAccessExpr&>(*expr.callee);

        // 检查是否是 super 调用 / Check if this is a super call
        bool wasSuperCall = isSuperCall_;
        isSuperCall_ = false;

        llvm::Value* base = genExpr(*ma.base);
        if (!base) return nullptr;

        // super 调用：直接查找父类方法 / super call: directly look up parent class method
        if (wasSuperCall && currentSuperclassName_.size() > 0) {
            std::string parentMethodName = currentSuperclassName_ + "." + ma.member;
            llvm::Function* parentMethod = module_->getFunction(parentMethodName);
            if (parentMethod) {
                std::vector<llvm::Value*> args;
                args.push_back(base); // self
                for (const auto& arg : expr.args) {
                    llvm::Value* argVal = genExpr(*arg.value);
                    if (argVal) args.push_back(argVal);
                }
                return builder_->CreateCall(parentMethod, args,
                    parentMethod->getReturnType()->isVoidTy() ? "" : "super.call");
            }
        }

        // 查找基类型的 vtable / Find base type's vtable
        std::string baseTypeName;
        if (ma.base->exprKind == ExprKind::Identifier) {
            auto& baseId = static_cast<const IdentifierExpr&>(*ma.base);
            auto typeIt = namedTypes_.find(baseId.name);
            if (typeIt != namedTypes_.end() && typeIt->second->isStructTy()) {
                baseTypeName = typeIt->second->getStructName().str();
            }
        }

        auto vtableIt = vtables_.find(baseTypeName);
        if (vtableIt != vtables_.end() && vtableIt->second.vtableType) {
            // 通过 vtable 调用方法 / Call method through vtable
            auto methodIt = vtableIt->second.methodIndices.find(ma.member);
            if (methodIt != vtableIt->second.methodIndices.end()) {
                // 加载 vtable 指针（类的第一个字段）
                llvm::Value* vtablePtr = builder_->CreateStructGEP(
                    base->getType(), base, 0, "vtable.ptr");
                llvm::Value* vtable = builder_->CreateLoad(
                    llvm::PointerType::get(context_, 0), vtablePtr, "vtable");

                // 加载方法函数指针
                llvm::Value* methodPtr = builder_->CreateGEP(
                    vtableIt->second.vtableType, vtable,
                    {llvm::ConstantInt::get(llvm::Type::getInt32Ty(context_), 0),
                     llvm::ConstantInt::get(llvm::Type::getInt32Ty(context_), methodIt->second)},
                    "method.ptr");
                llvm::Value* methodFunc = builder_->CreateLoad(
                    llvm::PointerType::get(context_, 0), methodPtr, "method");

                // 生成参数
                std::vector<llvm::Value*> args;
                args.push_back(base); // self
                for (const auto& arg : expr.args) {
                    llvm::Value* argVal = genExpr(*arg.value);
                    if (argVal) args.push_back(argVal);
                }

                // 调用方法
                llvm::FunctionType* methodTy = vtableIt->second.methods[methodIt->second]->getFunctionType();
                return builder_->CreateCall(methodTy, methodFunc, args, "vcall");
            }
        }

        // 回退到直接函数调用 / Fallback to direct function call
        std::string methodName = baseTypeName + "." + ma.member;
        llvm::Function* method = module_->getFunction(methodName);
        if (method) {
            std::vector<llvm::Value*> args;
            args.push_back(base); // self
            for (const auto& arg : expr.args) {
                llvm::Value* argVal = genExpr(*arg.value);
                if (argVal) args.push_back(argVal);
            }
            return builder_->CreateCall(method, args, method->getReturnType()->isVoidTy() ? "" : "call");
        }
    }

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
            llvm::Type* retType = genericAst->returnType ?
                resolveType(genericAst->returnType.get()) :
                llvm::Type::getVoidTy(context_);

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
    for (size_t ai = 0; ai < expr.args.size(); ai++) {
        const auto& arg = expr.args[ai];
        llvm::Value* argVal = nullptr;

        // 检查是否是 inout 参数（&变量）/ Check if inout argument (&variable)
        bool isInOutArg = arg.value->exprKind == ExprKind::InOut;
        if (isInOutArg) {
            auto& inOut = static_cast<const InOutExpr&>(*arg.value);
            if (inOut.subExpr->exprKind == ExprKind::Identifier) {
                auto& id = static_cast<const IdentifierExpr&>(*inOut.subExpr);
                auto it = namedValues_.find(id.name);
                if (it != namedValues_.end()) {
                    argVal = it->second; // 传递 alloca 指针
                }
            }
        }
        if (!argVal) {
            argVal = genExpr(*arg.value);
        }
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
    if (!expr.base) return nullptr;

    // 生成基表达式 / Generate base expression
    llvm::Value* base = genExpr(*expr.base);
    if (!base) return nullptr;

    std::string memberName = expr.member;

    // 如果基表达式是结构体类型，使用 GEP 访问字段
    // If base is a struct type, use GEP to access field
    if (base->getType()->isStructTy()) {
        llvm::StructType* structTy = llvm::cast<llvm::StructType>(base->getType());
        // 查找字段索引 / Find field index
        // 遍历当前 CU 的声明找到对应的结构体定义
        if (currentCu_) {
            for (const auto& decl : currentCu_->declarations) {
                if (!decl) continue;
                if (decl->declKind == DeclKind::Struct) {
                    auto& sd = static_cast<const StructDecl&>(*decl);
                    if (sd.name == structTy->getName().str()) {
                        for (size_t i = 0; i < sd.members.size(); i++) {
                            if (sd.members[i] && sd.members[i]->declKind == DeclKind::Variable) {
                                auto& vd = static_cast<const VariableDecl&>(*sd.members[i]);
                                std::string fieldName;
                                if (vd.pattern && vd.pattern->patternKind == PatternKind::Identifier) {
                                    fieldName = static_cast<const IdentifierPattern*>(vd.pattern.get())->name;
                                }
                                if (fieldName == memberName) {
                                    llvm::Value* fieldPtr = builder_->CreateStructGEP(
                                        structTy, base, (unsigned)i, memberName + ".ptr");
                                    return builder_->CreateLoad(
                                        structTy->getElementType(i), fieldPtr, memberName);
                                }
                            }
                        }
                    }
                } else if (decl->declKind == DeclKind::Class) {
                    auto& cd = static_cast<const ClassDecl&>(*decl);
                    if (cd.name == structTy->getName().str()) {
                        for (size_t i = 0; i < cd.members.size(); i++) {
                            if (cd.members[i] && cd.members[i]->declKind == DeclKind::Variable) {
                                auto& vd = static_cast<const VariableDecl&>(*cd.members[i]);
                                std::string fieldName;
                                if (vd.pattern && vd.pattern->patternKind == PatternKind::Identifier) {
                                    fieldName = static_cast<const IdentifierPattern*>(vd.pattern.get())->name;
                                }
                                if (fieldName == memberName) {
                                    llvm::Value* fieldPtr = builder_->CreateStructGEP(
                                        structTy, base, (unsigned)i, memberName + ".ptr");
                                    return builder_->CreateLoad(
                                        structTy->getElementType(i), fieldPtr, memberName);
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // 如果基是指针（引用类型），先加载再 GEP
    // If base is a pointer (reference type), load then GEP
    if (base->getType()->isPointerTy()) {
        // 尝试通过 namedTypes_ 找到基类型
        if (expr.base->exprKind == ExprKind::Identifier) {
            auto& baseId = static_cast<const IdentifierExpr&>(*expr.base);
            auto typeIt = namedTypes_.find(baseId.name);
            if (typeIt != namedTypes_.end() && typeIt->second->isStructTy()) {
                llvm::StructType* structTy = llvm::cast<llvm::StructType>(typeIt->second);
                if (currentCu_) {
                    for (const auto& decl : currentCu_->declarations) {
                        if (!decl) continue;
                        if (decl->declKind == DeclKind::Struct) {
                            auto& sd = static_cast<const StructDecl&>(*decl);
                            if (sd.name == structTy->getName().str()) {
                                for (size_t i = 0; i < sd.members.size(); i++) {
                                    if (sd.members[i] && sd.members[i]->declKind == DeclKind::Variable) {
                                        auto& vd = static_cast<const VariableDecl&>(*sd.members[i]);
                                        std::string fieldName;
                                        if (vd.pattern && vd.pattern->patternKind == PatternKind::Identifier) {
                                            fieldName = static_cast<const IdentifierPattern*>(vd.pattern.get())->name;
                                        }
                                        if (fieldName == memberName) {
                                            llvm::Value* fieldPtr = builder_->CreateStructGEP(
                                                structTy, base, (unsigned)i, memberName + ".ptr");
                                            return builder_->CreateLoad(
                                                structTy->getElementType(i), fieldPtr, memberName);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // 尝试从命名值中查找 / Try to find from named values
    auto it = namedValues_.find(memberName);
    if (it != namedValues_.end()) {
        return builder_->CreateLoad(namedTypes_[memberName], it->second, memberName);
    }

    error(expr.loc, "cannot access member '" + memberName + "'");
    return nullptr;
}

llvm::Value* IRGenerator::genArrayLiteral(const ArrayLiteralExpr& expr) {
    // 创建 Array<T> 结构体 { data_ptr, count, capacity }
    // Create Array<T> struct { data_ptr, count, capacity }

    // 确定元素类型 / Determine element type
    llvm::Type* elemType = llvm::Type::getInt64Ty(context_); // 默认 i64
    if (!expr.elements.empty()) {
        llvm::Value* first = genExpr(*expr.elements[0]);
        if (first) elemType = first->getType();
    }

    // 创建数组结构体类型 / Create array struct type
    llvm::StructType* arrType = llvm::StructType::get(context_, {
        llvm::PointerType::get(context_, 0), // data pointer
        llvm::Type::getInt64Ty(context_),     // count
        llvm::Type::getInt64Ty(context_)      // capacity
    });

    // 计算元素数量 / Count elements
    int64_t elemCount = static_cast<int64_t>(expr.elements.size());

    // 分配数据缓冲区（在栈上） / Allocate data buffer (on stack)
    llvm::AllocaInst* dataBuf = builder_->CreateAlloca(elemType,
        llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), elemCount), "arr.data");

    // 存储每个元素 / Store each element
    for (size_t i = 0; i < expr.elements.size(); i++) {
        llvm::Value* elem = genExpr(*expr.elements[i]);
        if (!elem) continue;
        // 类型转换 / Type conversion
        if (elem->getType() != elemType) {
            if (elemType->isDoubleTy() && elem->getType()->isIntegerTy()) {
                elem = builder_->CreateSIToFP(elem, elemType, "arr.cast");
            } else if (elemType->isIntegerTy(64) && elem->getType()->isIntegerTy()) {
                elem = builder_->CreateSExt(elem, elemType, "arr.cast");
            }
        }
        llvm::Value* ptr = builder_->CreateGEP(elemType, dataBuf,
            llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), i), "arr.elem.ptr");
        builder_->CreateStore(elem, ptr);
    }

    // 创建数组结构体 / Create array struct
    llvm::AllocaInst* arrStruct = builder_->CreateAlloca(arrType, nullptr, "arr");
    llvm::Value* dataFieldPtr = builder_->CreateStructGEP(arrType, arrStruct, 0, "arr.data.field");
    llvm::Value* dataAsPtr = builder_->CreatePointerCast(dataBuf, llvm::PointerType::get(context_, 0));
    builder_->CreateStore(dataAsPtr, dataFieldPtr);

    llvm::Value* countFieldPtr = builder_->CreateStructGEP(arrType, arrStruct, 1, "arr.count.field");
    builder_->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), elemCount), countFieldPtr);

    llvm::Value* capFieldPtr = builder_->CreateStructGEP(arrType, arrStruct, 2, "arr.cap.field");
    builder_->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), elemCount), capFieldPtr);

    // 加载并返回结构体值 / Load and return struct value
    return builder_->CreateLoad(arrType, arrStruct, "arr.val");
}

llvm::Value* IRGenerator::genDictLiteral(const DictLiteralExpr& expr) {
    // 创建 Dictionary<K,V> 结构体 { data_ptr, count, capacity }
    // Create Dictionary struct { data_ptr, count, capacity }
    llvm::StructType* dictType = llvm::StructType::get(context_, {
        llvm::PointerType::get(context_, 0), // data pointer
        llvm::Type::getInt64Ty(context_),     // count
        llvm::Type::getInt64Ty(context_)      // capacity
    });

    // 创建字典结构体（空字典） / Create dict struct (empty dict)
    llvm::AllocaInst* dictStruct = builder_->CreateAlloca(dictType, nullptr, "dict");

    // data = null
    llvm::Value* dataFieldPtr = builder_->CreateStructGEP(dictType, dictStruct, 0, "dict.data.field");
    builder_->CreateStore(llvm::ConstantPointerNull::get(llvm::PointerType::get(context_, 0)), dataFieldPtr);

    // count = number of initial entries
    llvm::Value* countFieldPtr = builder_->CreateStructGEP(dictType, dictStruct, 1, "dict.count.field");
    builder_->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_),
        static_cast<int64_t>(expr.entries.size())), countFieldPtr);

    // capacity
    llvm::Value* capFieldPtr = builder_->CreateStructGEP(dictType, dictStruct, 2, "dict.cap.field");
    builder_->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_),
        static_cast<int64_t>(expr.entries.size())), capFieldPtr);

    return builder_->CreateLoad(dictType, dictStruct, "dict.val");
}

llvm::Value* IRGenerator::genSetLiteral(const SetLiteralExpr& expr) {
    // 创建 Set<T> 结构体 { data_ptr, count, capacity }
    // Create Set struct { data_ptr, count, capacity }
    llvm::StructType* setType = llvm::StructType::get(context_, {
        llvm::PointerType::get(context_, 0), // data pointer
        llvm::Type::getInt64Ty(context_),     // count
        llvm::Type::getInt64Ty(context_)      // capacity
    });

    // 确定元素类型 / Determine element type
    llvm::Type* elemType = llvm::Type::getInt64Ty(context_); // 默认 i64
    if (!expr.elements.empty()) {
        llvm::Value* first = genExpr(*expr.elements[0]);
        if (first) elemType = first->getType();
    }

    int64_t elemCount = static_cast<int64_t>(expr.elements.size());

    // 分配元素缓冲区 / Allocate element buffer
    llvm::AllocaInst* dataBuf = builder_->CreateAlloca(elemType,
        llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), elemCount), "set.data");

    // 存储每个元素 / Store each element
    for (size_t i = 0; i < expr.elements.size(); i++) {
        llvm::Value* elem = genExpr(*expr.elements[i]);
        if (!elem) continue;
        if (elem->getType() != elemType) {
            if (elemType->isDoubleTy() && elem->getType()->isIntegerTy()) {
                elem = builder_->CreateSIToFP(elem, elemType, "set.cast");
            } else if (elemType->isIntegerTy(64) && elem->getType()->isIntegerTy()) {
                elem = builder_->CreateSExt(elem, elemType, "set.cast");
            }
        }
        llvm::Value* ptr = builder_->CreateGEP(elemType, dataBuf,
            llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), i), "set.elem.ptr");
        builder_->CreateStore(elem, ptr);
    }

    // 创建集合结构体 / Create set struct
    llvm::AllocaInst* setStruct = builder_->CreateAlloca(setType, nullptr, "set");

    // data = pointer to buffer
    llvm::Value* dataFieldPtr = builder_->CreateStructGEP(setType, setStruct, 0, "set.data.field");
    llvm::Value* dataAsPtr = builder_->CreatePointerCast(dataBuf, llvm::PointerType::get(context_, 0));
    builder_->CreateStore(dataAsPtr, dataFieldPtr);

    // count = number of elements
    llvm::Value* countFieldPtr = builder_->CreateStructGEP(setType, setStruct, 1, "set.count.field");
    builder_->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), elemCount), countFieldPtr);

    // capacity = count
    llvm::Value* capFieldPtr = builder_->CreateStructGEP(setType, setStruct, 2, "set.cap.field");
    builder_->CreateStore(llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), elemCount), capFieldPtr);

    return builder_->CreateLoad(setType, setStruct, "set.val");
}

llvm::Value* IRGenerator::genTupleExpr(const TupleExpr& expr) {
    if (expr.elements.empty()) return llvm::ConstantStruct::getAnon(context_, {});

    // 收集所有元素的值和类型 / Collect all element values and types
    std::vector<llvm::Value*> elemValues;
    std::vector<llvm::Type*> elemTypes;
    for (const auto& elem : expr.elements) {
        llvm::Value* val = genExpr(*elem.value);
        if (!val) return nullptr;
        elemValues.push_back(val);
        elemTypes.push_back(val->getType());
    }

    // 创建匿名结构体类型 / Create anonymous struct type
    llvm::StructType* tupleType = llvm::StructType::get(context_, elemTypes);

    // 在栈上分配并存储 / Allocate on stack and store
    llvm::AllocaInst* tupleAlloc = builder_->CreateAlloca(tupleType, nullptr, "tuple");
    for (size_t i = 0; i < elemValues.size(); i++) {
        llvm::Value* fieldPtr = builder_->CreateStructGEP(tupleType, tupleAlloc, (unsigned)i, "tuple.field");
        builder_->CreateStore(elemValues[i], fieldPtr);
    }

    return builder_->CreateLoad(tupleType, tupleAlloc, "tuple.val");
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
    // 声明 snprintf 用于格式化 / Declare snprintf for formatting
    llvm::Function* snprintfFunc = module_->getFunction("snprintf");
    if (!snprintfFunc) {
        // int snprintf(char* str, size_t size, const char* format, ...)
        llvm::Type* ptrTy = llvm::PointerType::get(context_, 0);
        llvm::FunctionType* snprintfTy = llvm::FunctionType::get(
            llvm::Type::getInt32Ty(context_),
            {ptrTy, llvm::Type::getInt64Ty(context_), ptrTy}, true);
        snprintfFunc = llvm::Function::Create(snprintfTy, llvm::Function::ExternalLinkage,
                                              "snprintf", module_.get());
    }

    // 分配结果缓冲区 / Allocate result buffer (1024 bytes should be enough for most cases)
    llvm::AllocaInst* buf = builder_->CreateAlloca(
        llvm::Type::getInt8Ty(context_),
        llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), 1024), "interp.buf");

    // 构建格式字符串和参数 / Build format string and arguments
    std::string fmtStr;
    std::vector<llvm::Value*> args;
    args.push_back(buf);
    args.push_back(llvm::ConstantInt::get(llvm::Type::getInt64Ty(context_), 1024));

    for (const auto& seg : expr.segments) {
        if (!seg.expression) {
            // 纯字面量文本 / Pure literal text
            fmtStr += seg.literalText;
        } else {
            // 这是一个表达式 / This is an expression
            // 先添加之前累积的字面量到格式字符串
            // 将表达式值转换为格式说明符
            llvm::Value* exprVal = genExpr(*seg.expression);
            if (!exprVal) continue;

            if (exprVal->getType()->isIntegerTy(64)) {
                fmtStr += "%lld";
            } else if (exprVal->getType()->isIntegerTy(1)) {
                fmtStr += "%s";
                // 需要将 bool 转为字符串
                llvm::Value* trueStr = createStringGlobal("true");
                llvm::Value* falseStr = createStringGlobal("false");
                exprVal = builder_->CreateSelect(exprVal, trueStr, falseStr);
            } else if (exprVal->getType()->isDoubleTy()) {
                fmtStr += "%g";
            } else if (exprVal->getType()->isFloatTy()) {
                fmtStr += "%g";
                exprVal = builder_->CreateFPExt(exprVal, llvm::Type::getDoubleTy(context_), "fpext");
            } else if (exprVal->getType()->isPointerTy()) {
                fmtStr += "%s";
            } else if (exprVal->getType()->isIntegerTy()) {
                fmtStr += "%lld";
                exprVal = builder_->CreateSExt(exprVal, llvm::Type::getInt64Ty(context_), "sext");
            } else {
                fmtStr += "%s";
                exprVal = createStringGlobal("?");
            }
            args.push_back(exprVal);
        }
    }

    // 添加格式字符串参数 / Add format string argument
    llvm::Value* fmtGlobal = createStringGlobal(fmtStr);
    args.insert(args.begin() + 2, fmtGlobal);

    // 调用 snprintf / Call snprintf
    builder_->CreateCall(snprintfFunc, args);

    return buf;
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

    // deinit 会在 ARC 运行时中引用计数归零时自动调用
    // deinit is called by ARC runtime when reference count reaches zero
    // 这里生成的 deinit 函数通过 deinitFuncs_ 注册表供运行时使用
}

bool IRGenerator::isReferenceType(llvm::Type* type) const {
    // 指针类型视为引用类型
    return type->isPointerTy();
}

// ─── 宏支持 / Macro support ─────────────────────────────────────────────

void IRGenerator::processMacroDecl(const MacroDecl& decl) {
    // 注册宏定义 / Register macro definition
    macroExpander_.registerMacro(decl);
}

llvm::Value* IRGenerator::genMacroExpansion(const MacroExpansionExpr& expr) {
    // 展开宏 / Expand macro
    const MacroDecl* expanded = macroExpander_.expandMacro(expr);

    if (!expanded) {
        return nullptr;
    }

    // 生成展开后的语句 / Generate expanded statements
    if (!expanded->expansion.empty()) {
        for (const auto& stmt : expanded->expansion) {
            if (stmt) genStmt(*stmt);
        }
    } else {
        for (const auto& stmt : expanded->body) {
            if (stmt) genStmt(*stmt);
        }
    }

    return nullptr;
}

#endif

} // namespace suki
