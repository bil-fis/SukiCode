// SukiCode semantic analyzer implementation.
// Performs type checking, scope resolution, and validation on the parsed AST.

#include "Sema.h"
#include <fstream>
#include <set>

namespace suki {

Sema::Sema(DiagnosticEngine& diag) : diag_(diag), typeChecker_(diag, symbols_) {}
Sema::~Sema() = default;

bool Sema::analyze(CompilationUnit& cu) {
    currentCu_ = &cu;

    // Register built-in types
    auto regType = [&](const char* name) {
        Symbol sym;
        sym.kind = SymbolKind::Type;
        sym.name = name;
        sym.type = resolvePrimitiveType(name);
        sym.isPublic = true;
        symbols_.define(sym);
    };
    regType("Void");
    regType("Bool");
    regType("Int");
    regType("Int8");
    regType("Int16");
    regType("Int32");
    regType("Int64");
    regType("UInt");
    regType("UInt8");
    regType("UInt16");
    regType("UInt32");
    regType("UInt64");
    regType("Float");
    regType("Double");
    regType("Char");
    regType("String");

    // Register built-in functions
    {
        Symbol s;
        s.kind = SymbolKind::Function;
        s.name = "print";
        s.returnType = getVoidType();
        s.isPublic = true;
        s.paramTypes.push_back(getAnyType());
        symbols_.define(s);
    }

    if (cu.moduleDecl) {
        currentModule_ = cu.moduleDecl->name;
    }

    // 第零遍：预注册所有类型声明
    // Zeroth pass: pre-register all type declarations
    for (auto& decl : cu.declarations) {
        if (!decl) continue;
        if (decl->declKind == DeclKind::Struct ||
            decl->declKind == DeclKind::Class ||
            decl->declKind == DeclKind::Enum ||
            decl->declKind == DeclKind::Protocol ||
            decl->declKind == DeclKind::Actor) {
            auto& td = static_cast<TypeDecl&>(*decl);
            // 命名规范检查 / Naming convention check
            checkNamingConvention(td.name, true, td.loc);
            Symbol* existing = symbols_.lookup(td.name);
            if (!existing) {
                Symbol sym;
                sym.kind = SymbolKind::Type;
                sym.name = td.name;
                sym.isPublic = (td.access == AccessLevel::Public);
                symbols_.define(sym);
            }
        }
    }

    // 第一遍：预注册所有函数声明（支持前向引用）
    // First pass: pre-register all function declarations (support forward references)
    for (auto& decl : cu.declarations) {
        if (decl && decl->declKind == DeclKind::Function) {
            auto& fd = static_cast<FunctionDecl&>(*decl);
            Symbol* existing = symbols_.lookup(fd.name);
            if (!existing) {
                // 临时注册泛型参数以解析类型 / Temporarily register generic params for type resolution
                symbols_.enterScope();
                for (const auto& gp : fd.genericParams) {
                    Symbol gs;
                    gs.kind = SymbolKind::Type;
                    gs.name = gp.name;
                    gs.type = getAnyType();
                    symbols_.define(gs);
                }

                Symbol sym;
                sym.kind = SymbolKind::Function;
                sym.name = fd.name;
                sym.isPublic = (fd.access == AccessLevel::Public);

                // 解析参数类型（泛型参数在此作用域内可用）
                // Resolve parameter types (generic params available in this scope)
                for (const auto& param : fd.params) {
                    if (param.type) {
                        sym.paramTypes.push_back(resolveTypeRepr(*param.type));
                    } else {
                        sym.paramTypes.push_back(getErrorType());
                    }
                }
                if (fd.returnType) {
                    sym.returnType = resolveTypeRepr(*fd.returnType);
                } else {
                    sym.returnType = getVoidType();
                }

                // 离开临时作用域 / Leave temporary scope
                symbols_.leaveScope();

                // 在外部作用域定义函数 / Define function in outer scope
                symbols_.define(sym);
            }
        }
    }

    // 第二遍：处理所有声明
    // Second pass: process all declarations
    for (auto& decl : cu.declarations) {
        if (decl) processDecl(*decl);
    }

    return !diag_.hadErrors();
}

void Sema::processDecl(Decl& decl) {
    switch (decl.declKind) {
        case DeclKind::Module: break;
        case DeclKind::Import: {
            // 注册导入模块 / Register imported module
            auto& imp = static_cast<ImportDecl&>(decl);
            // 记录导入的模块名 / Record imported module name
            importedModules_.push_back(imp.moduleName);

            // 尝试查找模块文件 / Try to find module file
            // 搜索路径：当前目录、标准库路径
            // Search paths: current directory, stdlib paths
            std::vector<std::string> searchPaths = {
                imp.moduleName + ".suki",
                "src/stdlib/" + imp.moduleName + ".suki",
                "Sources/" + imp.moduleName + "/" + imp.moduleName + ".suki",
            };

            bool found = false;
            for (const auto& path : searchPaths) {
                std::ifstream testFile(path);
                if (testFile.good()) {
                    found = true;
                    // 模块文件存在，记录路径 / Module file exists, record path
                    break;
                }
            }

            if (!found) {
                // 模块未找到但不报错（可能是内置模块）/ Module not found but don't error (may be built-in)
                // stdlib 模块如 Core、System 等通过头文件提供
                // stdlib modules like Core, System etc. are provided via headers
            }
            break;
        }
        case DeclKind::Variable:
            processVariableDecl(static_cast<VariableDecl&>(decl));
            break;
        case DeclKind::Function:
            processFunctionDecl(static_cast<FunctionDecl&>(decl));
            break;
        case DeclKind::Struct:
            processStructDecl(static_cast<StructDecl&>(decl));
            break;
        case DeclKind::Class:
            processClassDecl(static_cast<ClassDecl&>(decl));
            break;
        case DeclKind::Enum:
            processEnumDecl(static_cast<EnumDecl&>(decl));
            break;
        case DeclKind::Extension: {
            // Extension: 处理扩展体内的成员
            auto& ext = static_cast<ExtensionDecl&>(decl);
            symbols_.enterScope();
            for (auto& member : ext.members) {
                if (member) processDecl(*member);
            }
            symbols_.leaveScope();
            break;
        }
        case DeclKind::Protocol: {
            // Protocol: 注册协议类型
            auto& proto = static_cast<ProtocolDecl&>(decl);
            Symbol sym;
            sym.kind = SymbolKind::Type;
            sym.name = proto.name;
            sym.isPublic = (proto.access == AccessLevel::Public);
            symbols_.define(sym);
            break;
        }
        case DeclKind::Actor: {
            // Actor: 处理为类类型，注册为 Actor
            auto& actor = static_cast<ActorDecl&>(decl);
            Symbol sym;
            sym.kind = SymbolKind::Type;
            sym.name = actor.name;
            sym.isPublic = (actor.access == AccessLevel::Public);
            symbols_.define(sym);
            // 注册 Actor 类型 / Register actor type
            actorTypes_.insert(actor.name);
            // 进入 Actor 作用域 / Enter actor scope
            std::string prevActor = currentActor_;
            currentActor_ = actor.name;
            symbols_.enterScope();
            for (auto& member : actor.members) {
                if (member) processDecl(*member);
            }
            symbols_.leaveScope();
            currentActor_ = prevActor;
            break;
        }
        case DeclKind::Typealias: {
            // Typealias: 注册类型别名
            auto& ta = static_cast<TypealiasDecl&>(decl);
            Symbol sym;
            sym.kind = SymbolKind::Type;
            sym.name = ta.name;
            sym.isPublic = (ta.access == AccessLevel::Public);
            if (ta.underlyingType) {
                sym.type = resolveTypeRepr(*ta.underlyingType);
            }
            symbols_.define(sym);
            break;
        }
        case DeclKind::Init: {
            // Init: 注册为函数
            auto& init = static_cast<InitDecl&>(decl);
            Symbol sym;
            sym.kind = SymbolKind::Function;
            sym.name = "init";
            sym.isPublic = true;
            sym.returnType = getVoidType();
            symbols_.define(sym);
            // 处理函数体
            symbols_.enterScope();
            for (auto& stmt : init.body) {
                if (stmt) processStmt(*stmt);
            }
            symbols_.leaveScope();
            break;
        }
        case DeclKind::Deinit: {
            // Deinit: 处理函数体
            auto& deinit = static_cast<DeinitDecl&>(decl);
            symbols_.enterScope();
            for (auto& stmt : deinit.body) {
                if (stmt) processStmt(*stmt);
            }
            symbols_.leaveScope();
            break;
        }
        case DeclKind::Subscript: {
            // Subscript: 注册为函数
            auto& sub = static_cast<SubscriptDecl&>(decl);
            Symbol sym;
            sym.kind = SymbolKind::Function;
            sym.name = "subscript";
            sym.isPublic = true;
            if (sub.returnType) {
                sym.returnType = resolveTypeRepr(*sub.returnType);
            }
            symbols_.define(sym);
            break;
        }
        case DeclKind::While: {
            auto& w = static_cast<WhileDecl&>(decl);
            typeChecker_.enterLoop();
            if (w.condition) {
                TypePtr condType = inferExprType(*w.condition);
                if (condType && condType->kind() != TypeKind::Bool && condType->kind() != TypeKind::Error) {
                    error(w.condition->loc, "while condition must be of type 'Bool'");
                }
            }
            symbols_.enterScope();
            for (auto& s : w.body) { if (s) processStmt(*s); }
            symbols_.leaveScope();
            typeChecker_.leaveLoop();
            break;
        }
        case DeclKind::ForIn: {
            auto& f = static_cast<ForInDecl&>(decl);
            typeChecker_.enterLoop();
            if (f.sequence) inferExprType(*f.sequence);
            symbols_.enterScope();
            for (auto& s : f.body) { if (s) processStmt(*s); }
            symbols_.leaveScope();
            typeChecker_.leaveLoop();
            break;
        }
        case DeclKind::RepeatWhile: {
            auto& r = static_cast<RepeatWhileDecl&>(decl);
            typeChecker_.enterLoop();
            symbols_.enterScope();
            for (auto& s : r.body) { if (s) processStmt(*s); }
            symbols_.leaveScope();
            if (r.condition) {
                TypePtr condType = inferExprType(*r.condition);
                if (condType && condType->kind() != TypeKind::Bool && condType->kind() != TypeKind::Error) {
                    error(r.condition->loc, "repeat-while condition must be of type 'Bool'");
                }
            }
            typeChecker_.leaveLoop();
            break;
        }
        case DeclKind::Switch: {
            auto& sw = static_cast<SwitchDecl&>(decl);
            typeChecker_.enterSwitch();
            TypePtr subjectType;
            if (sw.subject) subjectType = inferExprType(*sw.subject);

            // 检查是否有 default 分支 / Check for default branch
            bool hasDefault = false;
            for (const auto& c : sw.cases) {
                for (const auto& label : c.labels) {
                    if (label.isDefault) hasDefault = true;
                }
            }

            // 如果主体是枚举类型，检查穷举性
            // If subject is enum type, check exhaustiveness
            if (subjectType && subjectType->kind() == TypeKind::Enum && !hasDefault) {
                // 检查是否所有枚举 case 都被覆盖
                auto& enumType = static_cast<const EnumType&>(*subjectType);
                std::set<std::string> coveredCases;
                for (const auto& c : sw.cases) {
                    for (const auto& label : c.labels) {
                        if (label.expression && label.expression->exprKind == ExprKind::Identifier) {
                            coveredCases.insert(static_cast<const IdentifierExpr&>(*label.expression).name);
                        }
                    }
                }
                for (const auto& ec : enumType.cases()) {
                    if (coveredCases.find(ec.name) == coveredCases.end()) {
                        warning(decl.loc, "switch on enum '" + enumType.name() +
                                "' does not handle case '" + ec.name + "'");
                    }
                }
            } else if (!hasDefault) {
                // 非枚举类型且没有 default，发出警告
                warning(decl.loc, "switch without default case may not be exhaustive");
            }

            for (auto& c : sw.cases) {
                symbols_.enterScope();
                for (auto& s : c.body) { if (s) processStmt(*s); }
                symbols_.leaveScope();
            }
            typeChecker_.leaveSwitch();
            break;
        }
        case DeclKind::DoCatch: {
            auto& dc = static_cast<DoCatchDecl&>(decl);
            symbols_.enterScope();
            for (auto& s : dc.doBody) { if (s) processStmt(*s); }
            symbols_.leaveScope();
            for (auto& c : dc.catches) {
                symbols_.enterScope();
                for (auto& s : c.body) { if (s) processStmt(*s); }
                symbols_.leaveScope();
            }
            break;
        }
        case DeclKind::If: {
            auto& ifDecl = static_cast<IfDecl&>(decl);
            if (ifDecl.condition) {
                TypePtr condType = inferExprType(*ifDecl.condition);
                if (condType && condType->kind() != TypeKind::Bool && condType->kind() != TypeKind::Error) {
                    error(ifDecl.condition->loc, "if condition must be of type 'Bool'");
                }
            }
            symbols_.enterScope();
            for (auto& s : ifDecl.thenBody) { if (s) processStmt(*s); }
            symbols_.leaveScope();
            symbols_.enterScope();
            for (auto& s : ifDecl.elseBody) { if (s) processStmt(*s); }
            symbols_.leaveScope();
            break;
        }
        case DeclKind::Guard: {
            auto& g = static_cast<GuardDecl&>(decl);
            if (g.condition) {
                TypePtr condType = inferExprType(*g.condition);
                if (condType && condType->kind() != TypeKind::Bool && condType->kind() != TypeKind::Error) {
                    error(g.condition->loc, "guard condition must be of type 'Bool'");
                }
            }
            symbols_.enterScope();
            for (auto& s : g.elseBody) { if (s) processStmt(*s); }
            symbols_.leaveScope();
            break;
        }
        case DeclKind::Unsafe: {
            auto& u = static_cast<UnsafeDecl&>(decl);
            typeChecker_.enterUnsafe();
            symbols_.enterScope();
            for (auto& s : u.body) { if (s) processStmt(*s); }
            symbols_.leaveScope();
            typeChecker_.leaveUnsafe();
            break;
        }
        case DeclKind::Asm: {
            // 内联汇编必须在 unsafe 块内 / Inline assembly must be in unsafe block
            if (!typeChecker_.isInUnsafe()) {
                error(decl.loc, "inline assembly (asm) must be inside an unsafe block");
            }
            break;
        }
        case DeclKind::ExternBlock: {
            // extern 块：处理所有外部声明 / extern block: process all external declarations
            auto& eb = static_cast<ExternBlockDecl&>(decl);
            for (auto& d : eb.declarations) {
                if (d) processDecl(*d);
            }
            break;
        }
        case DeclKind::AssociatedType: {
            // 关联类型声明：注册到符号表
            auto& at = static_cast<AssociatedTypeDecl&>(decl);
            Symbol sym;
            sym.kind = SymbolKind::Type;
            sym.name = at.name;
            sym.isPublic = true;
            symbols_.define(sym);
            break;
        }
        case DeclKind::Macro: {
            // 宏声明：注册宏名称到符号表 / Macro declaration: register macro name
            auto& macroDecl = static_cast<MacroDecl&>(decl);
            Symbol sym;
            sym.kind = SymbolKind::Function; // 宏作为函数类型注册
            sym.name = macroDecl.name;
            sym.isPublic = (decl.access == AccessLevel::Public);
            sym.isInitialized = true;
            symbols_.define(sym);
            break;
        }
        case DeclKind::MacroExpansion: {
            // 宏展开：在语义分析阶段记录，代码生成阶段展开
            break;
        }
        default: break;
    }
}

void Sema::processVariableDecl(VariableDecl& decl) {
    Symbol sym;
    sym.kind = SymbolKind::Variable;
    sym.isConstant = decl.isLet;
    sym.line = decl.loc.line;
    sym.column = decl.loc.column;

    if (decl.pattern && decl.pattern->patternKind == PatternKind::Identifier) {
        auto* idPat = static_cast<IdentifierPattern*>(decl.pattern.get());
        sym.name = idPat->name;
    } else {
        return;
    }

    if (decl.typeAnnotation) {
        sym.type = resolveTypeRepr(*decl.typeAnnotation);
    }

    if (decl.initializer) {
        TypePtr initType = inferExprType(*decl.initializer);
        if (initType) {
            if (!sym.type) {
                sym.type = initType;
            } else if (initType->kind() != TypeKind::Error &&
                       !initType->canImplicitlyConvertTo(*sym.type)) {
                error(decl.loc, "type mismatch: cannot convert " +
                      initType->name() + " to " + sym.type->name());
            }
        }
        sym.isInitialized = true;
    }

    if (!sym.type) {
        error(decl.loc, "cannot infer type for variable '" + sym.name + "'");
        sym.type = getErrorType();
    }

    if (!symbols_.define(sym)) {
        error(decl.loc, "variable '" + sym.name + "' is already defined in this scope");
    }
}

void Sema::processFunctionDecl(FunctionDecl& decl) {
    // 处理属性 / Process attributes
    bool isMain = false;
    bool isCDecl = false;
    bool isNoMangle = false;
    for (const auto& attr : decl.attributes) {
        // 属性名可能包含 @ 前缀 / Attribute name may include @ prefix
        std::string name = attr.name;
        if (!name.empty() && name[0] == '@') name = name.substr(1);
        if (name == "main") isMain = true;
        if (name == "_cdecl") isCDecl = true;
        if (name == "no_mangle") isNoMangle = true;
    }
    // 调试：检查属性是否被检测到

    // @main 函数特殊处理 / @main function special handling
    if (isMain) {
        // @main 函数必须无参数，返回 Int 或 Void
        if (!decl.params.empty()) {
            warning(decl.loc, "@main function should have no parameters");
        }
        // 重命名为 main
        decl.name = "main";
    }

    Symbol sym;
    sym.kind = SymbolKind::Function;
    sym.name = decl.name;
    sym.line = decl.loc.line;
    sym.column = decl.loc.column;
    sym.isPublic = (decl.access == AccessLevel::Public) || isMain || isCDecl;

    // 命名规范检查 / Naming convention check
    checkNamingConvention(decl.name, false, decl.loc);

    // 进入临时作用域以注册泛型参数 / Enter temporary scope for generic params
    symbols_.enterScope();
    for (const auto& gp : decl.genericParams) {
        Symbol gs;
        gs.kind = SymbolKind::Type;
        gs.name = gp.name;
        gs.type = getAnyType();
        symbols_.define(gs);

        // 验证约束类型存在 / Verify constraint types exist
        for (const auto& constraint : gp.constraints) {
            if (constraint && constraint->typeReprKind == TypeReprKind::Named) {
                auto& ntr = static_cast<const NamedTypeRepr&>(*constraint);
                Symbol* constraintSym = symbols_.lookup(ntr.name);
                if (!constraintSym) {
                    warning(decl.loc, "generic constraint type '" + ntr.name + "' not found");
                }
            }
        }
    }

    // 验证 where 子句约束类型存在 / Verify where clause constraint types exist
    for (const auto& wc : decl.whereConstraints) {
        for (const auto& constraint : wc.constraints) {
            if (constraint && constraint->typeReprKind == TypeReprKind::Named) {
                auto& ntr = static_cast<const NamedTypeRepr&>(*constraint);
                Symbol* constraintSym = symbols_.lookup(ntr.name);
                if (!constraintSym) {
                    warning(decl.loc, "where clause constraint type '" + ntr.name + "' not found");
                }
            }
        }
    }

    // 解析参数类型（泛型参数在此作用域内可用）
    // Resolve parameter types (generic params available in this scope)
    for (const auto& param : decl.params) {
        if (param.type) {
            sym.paramTypes.push_back(resolveTypeRepr(*param.type));
        } else {
            sym.paramTypes.push_back(getErrorType());
        }
    }

    if (decl.returnType) {
        sym.returnType = resolveTypeRepr(*decl.returnType);
    } else {
        sym.returnType = getVoidType();
    }

    // 离开临时作用域 / Leave temporary scope
    symbols_.leaveScope();

    sym.type = std::make_shared<FunctionType>(
        std::vector<FunctionType::Param>(), sym.returnType, decl.isAsync, decl.isThrows);

    // 函数已在预注册阶段定义，跳过重复定义
    // Function already defined in pre-registration, skip duplicate
    symbols_.define(sym); // 允许更新（不报错）

    symbols_.enterScope();

    // 注册泛型类型参数 / Register generic type parameters
    for (const auto& gp : decl.genericParams) {
        Symbol gs;
        gs.kind = SymbolKind::Type;
        gs.name = gp.name;
        gs.type = getAnyType(); // 泛型参数用 Any 类型表示
        symbols_.define(gs);
    }

    // 设置函数上下文 / Set function context
    TypePtr prevReturnType = currentReturnType_;
    currentReturnType_ = sym.returnType;
    typeChecker_.setInThrowsFunction(decl.isThrows);
    typeChecker_.setInAsyncFunction(decl.isAsync);

    for (const auto& param : decl.params) {
        Symbol ps;
        ps.kind = SymbolKind::Variable;
        ps.name = param.internalName;
        ps.isConstant = true;
        if (param.type) ps.type = resolveTypeRepr(*param.type);
        ps.isInitialized = true;
        symbols_.define(ps);
    }

    for (auto& stmt : decl.body) {
        if (stmt) processStmt(*stmt);
    }

    // Restore previous return type
    currentReturnType_ = prevReturnType;

    symbols_.leaveScope();
}

void Sema::processStructDecl(StructDecl& decl) {
    // 类型已在预注册阶段定义，跳过重复定义
    std::string prevTypeName = currentTypeName_;
    currentTypeName_ = decl.name;

    symbols_.enterScope();
    for (auto& m : decl.members) {
        if (m) processDecl(*m);
    }
    symbols_.leaveScope();

    // 协议符合性检查 / Protocol conformance checking
    for (const auto& proto : decl.conformsTo) {
        if (!proto) continue;
        std::string protoName;
        if (proto->typeReprKind == TypeReprKind::Named) {
            protoName = static_cast<const NamedTypeRepr&>(*proto).name;
        }
        if (protoName.empty()) continue;

        // 查找协议定义 / Find protocol definition
        Symbol* protoSym = symbols_.lookup(protoName);
        if (!protoSym || protoSym->kind != SymbolKind::Type) {
            warning(decl.loc, "protocol '" + protoName + "' not found");
            continue;
        }

        // 收集协议要求的方法名、属性名、subscript / Collect required methods, properties, subscripts
        std::set<std::string> requiredMethods;
        std::set<std::string> requiredProperties;
        bool requiresSubscript = false;
        bool requiresInit = false;
        if (currentCu_) {
            for (const auto& d : currentCu_->declarations) {
                if (d && d->declKind == DeclKind::Protocol) {
                    auto& pd = static_cast<const ProtocolDecl&>(*d);
                    if (pd.name == protoName) {
                        for (const auto& member : pd.members) {
                            if (!member) continue;
                            if (member->declKind == DeclKind::Function) {
                                auto& fd = static_cast<const FunctionDecl&>(*member);
                                requiredMethods.insert(fd.name);
                            } else if (member->declKind == DeclKind::Variable) {
                                auto& vd = static_cast<const VariableDecl&>(*member);
                                if (vd.pattern && vd.pattern->patternKind == PatternKind::Identifier) {
                                    requiredProperties.insert(static_cast<const IdentifierPattern*>(vd.pattern.get())->name);
                                }
                            } else if (member->declKind == DeclKind::Subscript) {
                                requiresSubscript = true;
                            } else if (member->declKind == DeclKind::Init) {
                                requiresInit = true;
                            }
                        }
                        break;
                    }
                }
            }
        }

        // 检查当前类型是否实现了所有要求的方法
        for (const auto& methodName : requiredMethods) {
            bool found = false;
            for (const auto& member : decl.members) {
                if (member && member->declKind == DeclKind::Function) {
                    auto& fd = static_cast<const FunctionDecl&>(*member);
                    if (fd.name == methodName) {
                        found = true;
                        break;
                    }
                }
            }
            if (!found) {
                error(decl.loc, "type '" + decl.name + "' does not implement required method '" + methodName + "' from protocol '" + protoName + "'");
            }
        }

        // 检查当前类型是否实现了所有要求的属性
        for (const auto& propName : requiredProperties) {
            bool found = false;
            for (const auto& member : decl.members) {
                if (member && member->declKind == DeclKind::Variable) {
                    auto& vd = static_cast<const VariableDecl&>(*member);
                    if (vd.pattern && vd.pattern->patternKind == PatternKind::Identifier) {
                        if (static_cast<const IdentifierPattern*>(vd.pattern.get())->name == propName) {
                            found = true;
                            break;
                        }
                    }
                }
            }
            if (!found) {
                error(decl.loc, "type '" + decl.name + "' does not implement required property '" + propName + "' from protocol '" + protoName + "'");
            }
        }

        // 检查 subscript
        if (requiresSubscript) {
            bool found = false;
            for (const auto& member : decl.members) {
                if (member && member->declKind == DeclKind::Subscript) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                error(decl.loc, "type '" + decl.name + "' does not implement required subscript from protocol '" + protoName + "'");
            }
        }

        // 检查 init
        if (requiresInit) {
            bool found = false;
            for (const auto& member : decl.members) {
                if (member && member->declKind == DeclKind::Init) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                error(decl.loc, "type '" + decl.name + "' does not implement required init from protocol '" + protoName + "'");
            }
        }
    }

    currentTypeName_ = prevTypeName;
}

void Sema::processClassDecl(ClassDecl& decl) {
    // 类型已在预注册阶段定义，跳过重复定义
    std::string prevTypeName = currentTypeName_;
    std::string prevSuperclass = currentSuperclassName_;
    currentTypeName_ = decl.name;

    // 设置父类名称 / Set superclass name
    if (decl.superclass) {
        auto& ntr = static_cast<const NamedTypeRepr&>(*decl.superclass);
        currentSuperclassName_ = ntr.name;
        classParent_[decl.name] = ntr.name;
    } else {
        currentSuperclassName_.clear();
    }

    symbols_.enterScope();
    for (auto& m : decl.members) {
        if (!m) continue;

        // override/final 检查 / override/final checking
        if (m->declKind == DeclKind::Function) {
            auto& fd = static_cast<FunctionDecl&>(*m);
            if (fd.isOverride) {
                // 检查父类是否有该方法 / Check parent class has this method
                bool foundInParent = false;
                std::string parentName = currentSuperclassName_;
                while (!parentName.empty()) {
                    auto parentIt = classMethods_.find(parentName);
                    if (parentIt != classMethods_.end()) {
                        if (parentIt->second.count(fd.name)) {
                            foundInParent = true;
                            break;
                        }
                    }
                    auto ppIt = classParent_.find(parentName);
                    if (ppIt != classParent_.end()) {
                        parentName = ppIt->second;
                    } else {
                        break;
                    }
                }
                if (!foundInParent) {
                    error(fd.loc, "'" + fd.name + "' marked as 'override' but not found in parent class");
                }
            }
            if (fd.isFinal) {
                // 检查 final 方法不被子类重写（在子类处理时检查）
            }
            // 检查父类的 final 方法不被重写 / Check parent's final methods are not overridden
            std::string parentName = currentSuperclassName_;
            while (!parentName.empty()) {
                auto parentIt = classMethods_.find(parentName);
                if (parentIt != classMethods_.end()) {
                    auto methodIt = parentIt->second.find(fd.name);
                    if (methodIt != parentIt->second.end() && methodIt->second) {
                        error(fd.loc, "'" + fd.name + "' is final in parent class '" + parentName + "' and cannot be overridden");
                    }
                }
                auto ppIt = classParent_.find(parentName);
                if (ppIt != classParent_.end()) {
                    parentName = ppIt->second;
                } else {
                    break;
                }
            }
            // 记录方法 / Record method
            classMethods_[decl.name][fd.name] = fd.isFinal;
        } else if (m->declKind == DeclKind::Init) {
            auto& id = static_cast<InitDecl&>(*m);
            // required init 检查 / required init checking
            if (id.isRequired) {
                classMethods_[decl.name]["init.required"] = false;
            }
            // convenience init 检查 / convenience init checking
            if (id.isConvenience) {
                // convenience init 必须调用 self.init 或 super.init
                // 递归检查所有语句（包括嵌套块）
                bool callsSelfInit = false;
                std::function<void(const std::vector<StmtPtr>&)> checkStmts;
                checkStmts = [&](const std::vector<StmtPtr>& stmts) {
                    for (const auto& stmt : stmts) {
                        if (!stmt) continue;
                        if (stmt->stmtKind == StmtKind::Expression) {
                            auto& es = static_cast<const ExpressionStmt&>(*stmt);
                            if (es.expression && es.expression->exprKind == ExprKind::Call) {
                                auto& call = static_cast<const CallExpr&>(*es.expression);
                                if (call.callee->exprKind == ExprKind::MemberAccess) {
                                    auto& ma = static_cast<const MemberAccessExpr&>(*call.callee);
                                    if (ma.member == "init" &&
                                        (ma.base->exprKind == ExprKind::SelfRef ||
                                         ma.base->exprKind == ExprKind::SuperRef)) {
                                        callsSelfInit = true;
                                    }
                                }
                            }
                        } else if (stmt->stmtKind == StmtKind::DeclStmt) {
                            auto& ds = static_cast<const DeclStmt&>(*stmt);
                            if (ds.decl) {
                                // 检查嵌套控制流
                                if (ds.decl->declKind == DeclKind::If) {
                                    auto& ifDecl = static_cast<const IfDecl&>(*ds.decl);
                                    checkStmts(ifDecl.thenBody);
                                    checkStmts(ifDecl.elseBody);
                                } else if (ds.decl->declKind == DeclKind::DoCatch) {
                                    auto& dc = static_cast<const DoCatchDecl&>(*ds.decl);
                                    checkStmts(dc.doBody);
                                    for (const auto& c : dc.catches) {
                                        checkStmts(c.body);
                                    }
                                } else if (ds.decl->declKind == DeclKind::While) {
                                    auto& wh = static_cast<const WhileDecl&>(*ds.decl);
                                    checkStmts(wh.body);
                                } else if (ds.decl->declKind == DeclKind::ForIn) {
                                    auto& fi = static_cast<const ForInDecl&>(*ds.decl);
                                    checkStmts(fi.body);
                                }
                            }
                        } else if (stmt->stmtKind == StmtKind::Compound) {
                            auto& cs = static_cast<const CompoundStmt&>(*stmt);
                            checkStmts(cs.statements);
                        }
                    }
                };
                checkStmts(id.body);
                if (!callsSelfInit && !id.body.empty()) {
                    error(id.loc, "convenience init must delegate to another init via self.init() or super.init()");
                }
            }
        }

        processDecl(*m);
    }
    symbols_.leaveScope();

    // 检查父类的 required init 是否被实现 / Check parent's required inits are implemented
    if (!currentSuperclassName_.empty()) {
        std::string parentName = currentSuperclassName_;
        while (!parentName.empty()) {
            auto parentIt = classMethods_.find(parentName);
            if (parentIt != classMethods_.end()) {
                if (parentIt->second.count("init.required")) {
                    // 检查当前类是否有 required init
                    bool hasRequiredInit = false;
                    for (const auto& m : decl.members) {
                        if (m && m->declKind == DeclKind::Init) {
                            auto& id = static_cast<const InitDecl&>(*m);
                            if (id.isRequired) {
                                hasRequiredInit = true;
                                break;
                            }
                        }
                    }
                    if (!hasRequiredInit) {
                        error(decl.loc, "class '" + decl.name + "' must implement required init from parent class '" + parentName + "'");
                    }
                }
            }
            auto ppIt = classParent_.find(parentName);
            if (ppIt != classParent_.end()) {
                parentName = ppIt->second;
            } else {
                break;
            }
        }
    }

    currentTypeName_ = prevTypeName;
    currentSuperclassName_ = prevSuperclass;
}

void Sema::processEnumDecl(EnumDecl& decl) {
    // 类型已在预注册阶段定义，跳过重复定义
    // 注册枚举 case
    bool isPublic = (decl.access == AccessLevel::Public);
    for (auto& c : decl.cases) {
        if (c) {
            Symbol cs;
            cs.kind = SymbolKind::EnumCase;
            cs.name = c->name;
            cs.isPublic = isPublic;
            symbols_.define(cs);
        }
    }
}

void Sema::processStmt(Stmt& stmt) {
    switch (stmt.stmtKind) {
        case StmtKind::Expression:
            processExprStmt(static_cast<ExpressionStmt&>(stmt));
            break;
        case StmtKind::Return:
            processReturnStmt(static_cast<ReturnStmt&>(stmt));
            break;
        case StmtKind::Break:
            if (!typeChecker_.isInLoop() && !typeChecker_.isInSwitch()) {
                error(stmt.loc, "'break' must be inside a loop or switch");
            }
            break;
        case StmtKind::Continue:
            if (!typeChecker_.isInLoop()) {
                error(stmt.loc, "'continue' must be inside a loop");
            }
            break;
        case StmtKind::Fallthrough:
            if (!typeChecker_.isInSwitch()) {
                error(stmt.loc, "'fallthrough' must be inside a switch case");
            }
            break;
        case StmtKind::Throw: {
            if (!typeChecker_.isInThrowsFunction()) {
                error(stmt.loc, "'throw' must be inside a function marked 'throws'");
            }
            // 检查 thrown 类型是否符合 Error 协议
            auto& throwStmt = static_cast<const ThrowStmt&>(stmt);
            if (throwStmt.value) {
                TypePtr thrownType = inferExprType(*throwStmt.value);
                if (thrownType) {
                    // 检查是否是 Error 类型或其子类
                    bool isErrorType = false;
                    if (thrownType->kind() == TypeKind::Error) {
                        isErrorType = true;
                    } else if (thrownType->kind() == TypeKind::Class || thrownType->kind() == TypeKind::Struct) {
                        // 检查类型名是否包含 "Error"
                        std::string typeName = thrownType->name();
                        if (typeName.find("Error") != std::string::npos) {
                            isErrorType = true;
                        }
                    }
                    if (!isErrorType && thrownType->kind() != TypeKind::Any) {
                        warning(stmt.loc, "thrown type '" + thrownType->name() + "' may not conform to Error protocol");
                    }
                }
            }
            break;
        }
        case StmtKind::VariableDecl: {
            auto& vs = static_cast<VariableDeclStmt&>(stmt);
            if (vs.varDecl) processVariableDecl(static_cast<VariableDecl&>(*vs.varDecl));
            break;
        }
        case StmtKind::DeclStmt: {
            auto& ds = static_cast<DeclStmt&>(stmt);
            if (ds.decl) processDecl(*ds.decl);
            break;
        }
        case StmtKind::Compound: {
            auto& cs = static_cast<CompoundStmt&>(stmt);
            symbols_.enterScope();
            for (auto& s : cs.statements) {
                if (s) processStmt(*s);
            }
            symbols_.leaveScope();
            break;
        }
        default: break;
    }
}

void Sema::processReturnStmt(ReturnStmt& stmt) {
    if (stmt.value) {
        TypePtr returnType = inferExprType(*stmt.value);
        if (returnType && currentReturnType_) {
            // 泛型参数返回类型跳过检查 / Skip check for generic return types
            if (returnType->kind() == TypeKind::Any) return;
            if (currentReturnType_->kind() == TypeKind::Any) return;
            typeChecker_.checkReturnType(*currentReturnType_, *returnType, stmt.loc);
        }
    } else if (currentReturnType_ && currentReturnType_->kind() != TypeKind::Void) {
        error(stmt.loc, "non-void function must return a value");
    }
}

void Sema::processExprStmt(ExpressionStmt& stmt) {
    if (stmt.expression) inferExprType(*stmt.expression);
}

TypePtr Sema::inferExprType(Expr& expr) {
    switch (expr.exprKind) {
        case ExprKind::IntegerLiteral:
            return getIntType(64);
        case ExprKind::FloatLiteral:
            return getDoubleType();
        case ExprKind::StringLiteral:
            return getStringType();
        case ExprKind::BoolLiteral:
            return getBoolType();
        case ExprKind::NilLiteral:
            // nil 可以赋值给任何 Optional 类型
            // nil can be assigned to any Optional type
            // 返回 ErrorType 作为特殊标记，在类型检查时允许赋值给 Optional
            return getErrorType();
        case ExprKind::Identifier: {
            auto& id = static_cast<IdentifierExpr&>(expr);
            Symbol* sym = symbols_.lookup(id.name);
            if (!sym) {
                error(expr.loc, "undeclared identifier '" + id.name + "'");
                return getErrorType();
            }
            // 移动语义检查 / Move semantics check
            if (movedVariables_.count(id.name) > 0) {
                error(expr.loc, "variable '" + id.name + "' has been moved and cannot be used");
                return getErrorType();
            }
            return sym->type;
        }
        case ExprKind::Binary: {
            auto& bin = static_cast<BinaryExpr&>(expr);
            TypePtr lt = inferExprType(*bin.left);
            TypePtr rt = inferExprType(*bin.right);
            if (!lt || !rt) return getErrorType();
            if (bin.op == TokenKind::Equal || bin.op == TokenKind::NotEqual ||
                bin.op == TokenKind::Less || bin.op == TokenKind::Greater ||
                bin.op == TokenKind::LessEqual || bin.op == TokenKind::GreaterEqual ||
                bin.op == TokenKind::AmpAmp || bin.op == TokenKind::PipePipe) {
                return getBoolType();
            }
            return lt;
        }
        case ExprKind::Call: {
            auto& call = static_cast<CallExpr&>(expr);

            // Actor 隔离检查：调用 actor 方法需要 await
            // Actor isolation check: calling actor methods requires await
            if (call.callee->exprKind == ExprKind::MemberAccess) {
                auto& ma = static_cast<MemberAccessExpr&>(*call.callee);
                TypePtr baseType = inferExprType(*ma.base);
                if (baseType && baseType->kind() == TypeKind::Actor) {
                    // 检查调用是否被 await 包裹
                    if (!isInAwaitExpr_) {
                        error(expr.loc, "calling actor method '" + ma.member + "' requires 'await'");
                    }
                }
            }

            if (call.callee->exprKind == ExprKind::Identifier) {
                auto& id = static_cast<IdentifierExpr&>(*call.callee);
                Symbol* sym = symbols_.lookup(id.name);
                if (!sym) {
                    error(expr.loc, "undeclared function '" + id.name + "'");
                    return getErrorType();
                }
                if (sym->kind != SymbolKind::Function) {
                    error(expr.loc, "'" + id.name + "' is not a function");
                    return getErrorType();
                }
                // Check argument count
                if (call.args.size() != sym->paramTypes.size()) {
                    error(expr.loc, "function '" + id.name + "' expects " +
                          std::to_string(sym->paramTypes.size()) + " arguments, got " +
                          std::to_string(call.args.size()));
                } else {
                    // Check argument types
                    for (size_t i = 0; i < call.args.size(); i++) {
                        TypePtr argType = inferExprType(*call.args[i].value);
                        if (argType && sym->paramTypes[i] &&
                            argType->kind() != TypeKind::Error &&
                            sym->paramTypes[i]->kind() != TypeKind::Error &&
                            !argType->canImplicitlyConvertTo(*sym->paramTypes[i])) {
                            error(call.args[i].value->loc,
                                  "argument type " + argType->name() +
                                  " does not match parameter type " + sym->paramTypes[i]->name());
                        }
                    }
                }
                return sym->returnType;
            }
            return nullptr;
        }
        case ExprKind::Unary: {
            auto& u = static_cast<UnaryExpr&>(expr);
            TypePtr ot = inferExprType(*u.operand);
            if (!ot) return getErrorType();
            if (u.op == TokenKind::Bang) return getBoolType();
            return ot;
        }
        case ExprKind::MemberAccess: {
            auto& ma = static_cast<MemberAccessExpr&>(expr);
            TypePtr baseType = inferExprType(*ma.base);
            if (!baseType) return nullptr;

            // 首先在当前作用域查找 / First look up in current scope
            Symbol* sym = symbols_.lookup(ma.member);
            if (sym) return sym->type;

            // 基于基类型查找成员 / Look up member based on base type
            // 如果基类型是命名类型，查找其成员
            if (baseType->kind() == TypeKind::Struct ||
                baseType->kind() == TypeKind::Class ||
                baseType->kind() == TypeKind::Enum) {
                // 在符号表中查找类型成员 / Look up type member in symbol table
                std::string typeName = baseType->name();
                Symbol* typeSym = symbols_.lookup(typeName);
                if (typeSym) {
                    // 查找成员函数 / Look up member function
                    std::string memberFuncName = typeName + "." + ma.member;
                    Symbol* memberSym = symbols_.lookup(memberFuncName);
                    if (memberSym) return memberSym->type;
                }
            }

            // 如果基类型是字符串，返回字符串方法的类型
            // If base type is String, return String method type
            if (baseType->kind() == TypeKind::String) {
                // 字符串方法如 count, isEmpty 等返回 Int 或 Bool
                if (ma.member == "count" || ma.member == "length") return getIntType();
                if (ma.member == "isEmpty") return getBoolType();
                if (ma.member == "uppercased" || ma.member == "lowercased") return getStringType();
            }

            // 如果基类型是数组，返回数组属性类型
            // If base type is Array, return Array property type
            if (baseType->kind() == TypeKind::Array) {
                if (ma.member == "count" || ma.member == "size") return getIntType();
                if (ma.member == "isEmpty") return getBoolType();
            }

            return nullptr;
        }
        case ExprKind::Subscript: {
            auto& sub = static_cast<SubscriptExpr&>(expr);
            TypePtr baseType = inferExprType(*sub.base);
            if (!baseType) return nullptr;
            // Array subscript returns element type
            if (baseType->kind() == TypeKind::Array) {
                return std::static_pointer_cast<ArrayType>(baseType)->elementType();
            }
            // Dictionary subscript returns value type
            if (baseType->kind() == TypeKind::Dictionary) {
                return std::static_pointer_cast<DictType>(baseType)->valueType();
            }
            return nullptr;
        }
        case ExprKind::OptionalChain: {
            auto& oc = static_cast<OptionalChainExpr&>(expr);
            TypePtr innerType = inferExprType(*oc.subExpr);
            if (!innerType) return nullptr;
            // Optional chain wraps result in Optional
            return std::make_shared<OptionalType>(innerType);
        }
        case ExprKind::ForceUnwrap: {
            auto& fu = static_cast<ForceUnwrapExpr&>(expr);
            TypePtr innerType = inferExprType(*fu.subExpr);
            if (!innerType) return nullptr;
            // Force unwrap removes Optional
            if (innerType->kind() == TypeKind::Optional) {
                return std::static_pointer_cast<OptionalType>(innerType)->baseType();
            }
            return innerType;
        }
        case ExprKind::TypeCast: {
            auto& tc = static_cast<TypeCastExpr&>(expr);
            return resolveTypeRepr(*tc.targetType);
        }
        case ExprKind::TypeCheck:
            return getBoolType(); // is Type always returns Bool
        case ExprKind::Try: {
            auto& te = static_cast<TryExpr&>(expr);
            TypePtr innerType = inferExprType(*te.subExpr);
            if (!innerType) return nullptr;
            // try? wraps in Optional, try! unwraps, try passes through
            if (te.isOptional) return std::make_shared<OptionalType>(innerType);
            return innerType;
        }
        case ExprKind::Await: {
            auto& ae = static_cast<AwaitExpr&>(expr);
            bool prevAwait = isInAwaitExpr_;
            isInAwaitExpr_ = true;
            TypePtr result = inferExprType(*ae.subExpr);
            isInAwaitExpr_ = prevAwait;
            return result;
        }
        case ExprKind::Move: {
            auto& me = static_cast<MoveExpr&>(expr);
            // 标记变量为已移动 / Mark variable as moved
            if (me.subExpr->exprKind == ExprKind::Identifier) {
                auto& id = static_cast<IdentifierExpr&>(*me.subExpr);
                movedVariables_.insert(id.name);
            }
            return inferExprType(*me.subExpr);
        }
        case ExprKind::ArrayLiteral: {
            auto& arr = static_cast<const ArrayLiteralExpr&>(expr);
            // 从第一个元素推断类型 / Infer type from first element
            TypePtr elemType = getAnyType();
            if (!arr.elements.empty()) {
                TypePtr firstType = inferExprType(*arr.elements[0]);
                if (firstType) elemType = firstType;
            }
            return std::make_shared<ArrayType>(elemType);
        }
        case ExprKind::DictLiteral: {
            auto& dict = static_cast<const DictLiteralExpr&>(expr);
            TypePtr keyType = getAnyType();
            TypePtr valType = getAnyType();
            if (!dict.entries.empty()) {
                TypePtr k = inferExprType(*dict.entries[0].key);
                TypePtr v = inferExprType(*dict.entries[0].value);
                if (k) keyType = k;
                if (v) valType = v;
            }
            return std::make_shared<DictType>(keyType, valType);
        }
        case ExprKind::SetLiteral: {
            auto& set = static_cast<const SetLiteralExpr&>(expr);
            TypePtr elemType = getAnyType();
            if (!set.elements.empty()) {
                TypePtr firstType = inferExprType(*set.elements[0]);
                if (firstType) elemType = firstType;
            }
            return std::make_shared<SetType>(elemType);
        }
        case ExprKind::Tuple: {
            auto& tuple = static_cast<const TupleExpr&>(expr);
            std::vector<TupleType::Element> elems;
            for (const auto& e : tuple.elements) {
                TupleType::Element te;
                te.label = e.label;
                te.type = e.value ? inferExprType(*e.value) : getAnyType();
                elems.push_back(te);
            }
            return std::make_shared<TupleType>(std::move(elems));
        }
        case ExprKind::Closure:
            // 闭包类型需要从上下文推断，返回 nullptr
            return nullptr;
        case ExprKind::If: {
            auto& ifExpr = static_cast<const IfExpr&>(expr);
            TypePtr thenType = ifExpr.thenExpr ? inferExprType(*ifExpr.thenExpr) : nullptr;
            TypePtr elseType = ifExpr.elseExpr ? inferExprType(*ifExpr.elseExpr) : nullptr;
            if (thenType) return thenType;
            if (elseType) return elseType;
            return nullptr;
        }
        case ExprKind::InterpolatedString:
            return getStringType();
        case ExprKind::SelfRef:
            // self 引用当前实例 / self references current instance
            // 返回当前类型（如果有）/ Return current type (if available)
            if (!currentTypeName_.empty()) {
                Symbol* typeSym = symbols_.lookup(currentTypeName_);
                if (typeSym) return typeSym->type;
            }
            return getAnyType();
        case ExprKind::SuperRef: {
            // super 引用父类 / super references parent class
            // 返回父类类型 / Return superclass type
            if (!currentSuperclassName_.empty()) {
                Symbol* typeSym = symbols_.lookup(currentSuperclassName_);
                if (typeSym) return typeSym->type;
            }
            // 如果没有父类，返回当前类型
            if (!currentTypeName_.empty()) {
                Symbol* typeSym = symbols_.lookup(currentTypeName_);
                if (typeSym) return typeSym->type;
            }
            return getAnyType();
        }
        case ExprKind::MacroExpansion: {
            // 宏展开类型：从宏定义推断 / Macro expansion type: infer from macro definition
            auto& me = static_cast<const MacroExpansionExpr&>(expr);
            // 简化实现：返回 Any 类型 / Simplified: return Any type
            // 实际应该分析宏展开结果的类型
            return getAnyType();
        }
        default:
            return nullptr;
    }
}

TypePtr Sema::resolveTypeRepr(const TypeRepr& tr) {
    switch (tr.typeReprKind) {
        case TypeReprKind::Named: {
            auto& n = static_cast<const NamedTypeRepr&>(tr);
            TypePtr resolved = resolvePrimitiveType(n.name);
            if (resolved) return resolved;
            Symbol* sym = symbols_.lookup(n.name);
            if (sym && sym->kind == SymbolKind::Type) return sym->type;
            error(tr.loc, "unknown type '" + n.name + "'");
            return getErrorType();
        }
        case TypeReprKind::Array: {
            auto& a = static_cast<const ArrayTypeRepr&>(tr);
            return std::make_shared<ArrayType>(resolveTypeRepr(*a.elementType));
        }
        case TypeReprKind::Dictionary: {
            auto& d = static_cast<const DictTypeRepr&>(tr);
            return std::make_shared<DictType>(resolveTypeRepr(*d.keyType),
                                               resolveTypeRepr(*d.valueType));
        }
        case TypeReprKind::Optional: {
            auto& o = static_cast<const OptionalTypeRepr&>(tr);
            return std::make_shared<OptionalType>(resolveTypeRepr(*o.base));
        }
        case TypeReprKind::Function: {
            auto& f = static_cast<const FunctionTypeRepr&>(tr);
            std::vector<FunctionType::Param> params;
            for (const auto& p : f.params) {
                FunctionType::Param fp;
                fp.type = resolveTypeRepr(*p.type);
                fp.isInOut = p.isInOut;
                params.push_back(fp);
            }
            TypePtr ret = f.returnType ? resolveTypeRepr(*f.returnType) : getVoidType();
            return std::make_shared<FunctionType>(params, ret, f.isAsync, f.isThrows);
        }
        case TypeReprKind::Tuple: {
            auto& t = static_cast<const TupleTypeRepr&>(tr);
            std::vector<TupleType::Element> elems;
            for (const auto& e : t.elements) {
                TupleType::Element te;
                te.label = e.label;
                te.type = resolveTypeRepr(*e.type);
                elems.push_back(te);
            }
            return std::make_shared<TupleType>(std::move(elems));
        }
        case TypeReprKind::Composition: {
            // A & B 组合类型 — 返回所有协议的联合类型
            // Composition type: return the primary protocol type
            auto& c = static_cast<const CompositionTypeRepr&>(tr);
            if (!c.protocols.empty()) {
                // 返回第一个协议类型（主协议）
                return resolveTypeRepr(*c.protocols[0]);
            }
            return getAnyType();
        }
        case TypeReprKind::Opaque: {
            // some P 不透明类型 — 返回约束类型
            auto& o = static_cast<const OpaqueTypeRepr&>(tr);
            return resolveTypeRepr(*o.constraint);
        }
        case TypeReprKind::Existential: {
            // any P 存在类型 — 返回 Any 类型
            return getAnyType();
        }
        case TypeReprKind::Owned: {
            // Owned<T> — 返回内部类型（布局相同）
            // 规范要求 T 必须是值类型（struct/enum/基本类型），不能是 class
            auto& o = static_cast<const OwnedTypeRepr&>(tr);
            TypePtr innerType = resolveTypeRepr(*o.inner);
            if (innerType) {
                // 检查是否为引用类型 / Check if reference type
                TypeKind kind = innerType->kind();
                if (kind == TypeKind::Class || kind == TypeKind::Actor) {
                    error(tr.loc, "Owned<T> cannot be used with class types; "
                          "use struct, enum, or primitive types instead");
                }
            }
            return innerType;
        }
        case TypeReprKind::Self: {
            // Self 类型 — 返回当前处理的类型
            // Self type: return the current type being processed
            if (!currentTypeName_.empty()) {
                Symbol* typeSym = symbols_.lookup(currentTypeName_);
                if (typeSym) return typeSym->type;
            }
            return getAnyType();
        }
        case TypeReprKind::Inferred:
            // _ 推断类型 — 返回 nullptr（让调用者推断）
            return nullptr;
        default:
            return nullptr;
    }
}

void Sema::error(SourceLocation loc, const std::string& msg) {
    diag_.error(loc, "", msg);
}

void Sema::warning(SourceLocation loc, const std::string& msg) {
    diag_.warning(loc, "", msg);
}

// ─── 代码风格检查 / Code style checking ────────────────────────────────

bool Sema::isUpperCamelCase(const std::string& name) const {
    if (name.empty()) return false;
    // 第一个字符必须是大写 / First character must be uppercase
    return std::isupper(name[0]);
}

bool Sema::isLowerCamelCase(const std::string& name) const {
    if (name.empty()) return false;
    // 第一个字符必须是小写或下划线 / First character must be lowercase or underscore
    return std::islower(name[0]) || name[0] == '_';
}

void Sema::checkNamingConvention(const std::string& name, bool isType, SourceLocation loc) {
    if (name.empty()) return;
    // 跳过特殊名称 / Skip special names
    if (name[0] == '_' || name == "main") return;

    if (isType) {
        if (!isUpperCamelCase(name)) {
            warning(loc, "type name '" + name + "' should use UpperCamelCase");
        }
    } else {
        if (!isLowerCamelCase(name)) {
            warning(loc, "name '" + name + "' should use lowerCamelCase");
        }
    }
}

} // namespace suki
