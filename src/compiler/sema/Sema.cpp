// SukiCode semantic analyzer implementation.
// Performs type checking, scope resolution, and validation on the parsed AST.

#include "Sema.h"

namespace suki {

Sema::Sema(DiagnosticEngine& diag) : diag_(diag), typeChecker_(diag, symbols_) {}
Sema::~Sema() = default;

bool Sema::analyze(CompilationUnit& cu) {
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

    for (auto& decl : cu.declarations) {
        if (decl) processDecl(*decl);
    }

    return !diag_.hadErrors();
}

void Sema::processDecl(Decl& decl) {
    switch (decl.declKind) {
        case DeclKind::Module: break;
        case DeclKind::Import: break;
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
            // Actor: 处理为类类型
            auto& actor = static_cast<ActorDecl&>(decl);
            Symbol sym;
            sym.kind = SymbolKind::Type;
            sym.name = actor.name;
            sym.isPublic = (actor.access == AccessLevel::Public);
            symbols_.define(sym);
            symbols_.enterScope();
            for (auto& member : actor.members) {
                if (member) processDecl(*member);
            }
            symbols_.leaveScope();
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
    Symbol sym;
    sym.kind = SymbolKind::Function;
    sym.name = decl.name;
    sym.line = decl.loc.line;
    sym.column = decl.loc.column;
    sym.isPublic = (decl.access == AccessLevel::Public);

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

    sym.type = std::make_shared<FunctionType>(
        std::vector<FunctionType::Param>(), sym.returnType, decl.isAsync, decl.isThrows);

    if (!symbols_.define(sym)) {
        error(decl.loc, "function '" + decl.name + "' is already defined");
    }

    symbols_.enterScope();

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
    Symbol sym;
    sym.kind = SymbolKind::Type;
    sym.name = decl.name;
    sym.line = decl.loc.line;
    sym.column = decl.loc.column;
    sym.isPublic = (decl.access == AccessLevel::Public);

    if (!symbols_.define(sym)) {
        error(decl.loc, "type '" + decl.name + "' is already defined");
    }

    symbols_.enterScope();
    for (auto& m : decl.members) {
        if (m) processDecl(*m);
    }
    symbols_.leaveScope();
}

void Sema::processClassDecl(ClassDecl& decl) {
    Symbol sym;
    sym.kind = SymbolKind::Type;
    sym.name = decl.name;
    sym.line = decl.loc.line;
    sym.column = decl.loc.column;
    sym.isPublic = (decl.access == AccessLevel::Public);

    if (!symbols_.define(sym)) {
        error(decl.loc, "type '" + decl.name + "' is already defined");
    }

    symbols_.enterScope();
    for (auto& m : decl.members) {
        if (m) processDecl(*m);
    }
    symbols_.leaveScope();
}

void Sema::processEnumDecl(EnumDecl& decl) {
    Symbol sym;
    sym.kind = SymbolKind::Type;
    sym.name = decl.name;
    sym.line = decl.loc.line;
    sym.column = decl.loc.column;
    sym.isPublic = (decl.access == AccessLevel::Public);

    if (!symbols_.define(sym)) {
        error(decl.loc, "type '" + decl.name + "' is already defined");
    }

    for (auto& c : decl.cases) {
        if (c) {
            Symbol cs;
            cs.kind = SymbolKind::EnumCase;
            cs.name = c->name;
            cs.isPublic = sym.isPublic;
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
            return nullptr;
        case ExprKind::Identifier: {
            auto& id = static_cast<IdentifierExpr&>(expr);
            Symbol* sym = symbols_.lookup(id.name);
            if (!sym) {
                error(expr.loc, "undeclared identifier '" + id.name + "'");
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
            // Look up member in the base type's scope
            // For now, look up as a function or variable
            Symbol* sym = symbols_.lookup(ma.member);
            if (sym) return sym->type;
            // TODO: proper member lookup based on base type
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
            return inferExprType(*ae.subExpr);
        }
        case ExprKind::Move: {
            auto& me = static_cast<MoveExpr&>(expr);
            return inferExprType(*me.subExpr);
        }
        case ExprKind::ArrayLiteral:
            return std::make_shared<ArrayType>(getAnyType());
        case ExprKind::DictLiteral:
            return std::make_shared<DictType>(getAnyType(), getAnyType());
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
            // A & B 组合类型 — 返回第一个协议类型（简化处理）
            auto& c = static_cast<const CompositionTypeRepr&>(tr);
            if (!c.protocols.empty()) {
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
            auto& o = static_cast<const OwnedTypeRepr&>(tr);
            return resolveTypeRepr(*o.inner);
        }
        case TypeReprKind::Self:
            // Self 类型 — 返回 Any（简化处理）
            return getAnyType();
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

} // namespace suki
