// SukiCode semantic analyzer implementation.
// Performs type checking, scope resolution, and validation on the parsed AST.

#include "Sema.h"

namespace suki {

Sema::Sema(DiagnosticEngine& diag) : diag_(diag) {}
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
        inferExprType(*stmt.value);
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
