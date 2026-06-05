// AST printer implementation — outputs a human-readable tree representation.

#include "ASTPrinter.h"

namespace suki {

void ASTPrinter::indent(int level) {
    for (int i = 0; i < level; i++) out_ << "  ";
}

std::string ASTPrinter::print(const CompilationUnit& cu) {
    out_.str("");
    out_.clear();
    out_ << "CompilationUnit {\n";
    if (cu.moduleDecl) {
        indent(1);
        out_ << "module: " << cu.moduleDecl->name << "\n";
    }
    for (const auto& decl : cu.declarations) {
        printDecl(*decl, 1);
    }
    out_ << "}\n";
    return out_.str();
}

void ASTPrinter::printDecl(const Decl& decl, int indentLevel) {
    indent(indentLevel);
    switch (decl.declKind) {
        case DeclKind::Module: {
            auto& d = static_cast<const ModuleDecl&>(decl);
            out_ << "ModuleDecl(\"" << d.name << "\")\n";
            break;
        }
        case DeclKind::Import: {
            auto& d = static_cast<const ImportDecl&>(decl);
            out_ << "ImportDecl(\"" << d.moduleName << "\")\n";
            break;
        }
        case DeclKind::Variable: {
            auto& d = static_cast<const VariableDecl&>(decl);
            out_ << (d.isLet ? "LetDecl" : "VarDecl") << " {\n";
            if (d.pattern) printPattern(*d.pattern, indentLevel + 1);
            if (d.typeAnnotation) {
                indent(indentLevel + 1); out_ << "type:\n";
                printTypeRepr(*d.typeAnnotation, indentLevel + 2);
            }
            if (d.initializer) {
                indent(indentLevel + 1); out_ << "init:\n";
                printExpr(*d.initializer, indentLevel + 2);
            }
            indent(indentLevel); out_ << "}\n";
            break;
        }
        case DeclKind::Function: {
            auto& d = static_cast<const FunctionDecl&>(decl);
            out_ << "FuncDecl(\"" << d.name << "\") {\n";
            indent(indentLevel + 1);
            out_ << "params: " << d.params.size() << "\n";
            if (d.returnType) {
                indent(indentLevel + 1); out_ << "returnType:\n";
                printTypeRepr(*d.returnType, indentLevel + 2);
            }
            indent(indentLevel + 1); out_ << "body: " << d.body.size() << " stmts\n";
            indent(indentLevel); out_ << "}\n";
            break;
        }
        case DeclKind::Struct: {
            auto& d = static_cast<const StructDecl&>(decl);
            out_ << "StructDecl(\"" << d.name << "\") {\n";
            for (const auto& m : d.members) printDecl(*m, indentLevel + 1);
            indent(indentLevel); out_ << "}\n";
            break;
        }
        case DeclKind::Class: {
            auto& d = static_cast<const ClassDecl&>(decl);
            out_ << "ClassDecl(\"" << d.name << "\") {\n";
            for (const auto& m : d.members) printDecl(*m, indentLevel + 1);
            indent(indentLevel); out_ << "}\n";
            break;
        }
        case DeclKind::Enum: {
            auto& d = static_cast<const EnumDecl&>(decl);
            out_ << "EnumDecl(\"" << d.name << "\") {\n";
            for (const auto& c : d.cases) {
                indent(indentLevel + 1);
                out_ << "case " << c->name << "\n";
            }
            indent(indentLevel); out_ << "}\n";
            break;
        }
        case DeclKind::Protocol: {
            auto& d = static_cast<const ProtocolDecl&>(decl);
            out_ << "ProtocolDecl(\"" << d.name << "\")\n";
            break;
        }
        case DeclKind::Actor: {
            auto& d = static_cast<const ActorDecl&>(decl);
            out_ << "ActorDecl(\"" << d.name << "\")\n";
            break;
        }
        case DeclKind::Extension: {
            auto& d = static_cast<const ExtensionDecl&>(decl);
            out_ << "ExtensionDecl {\n";
            for (const auto& m : d.members) printDecl(*m, indentLevel + 1);
            indent(indentLevel); out_ << "}\n";
            break;
        }
        case DeclKind::Typealias: {
            auto& d = static_cast<const TypealiasDecl&>(decl);
            out_ << "TypealiasDecl(\"" << d.name << "\")\n";
            break;
        }
        case DeclKind::If: {
            auto& d = static_cast<const IfDecl&>(decl);
            out_ << "IfDecl {\n";
            indent(indentLevel + 1); out_ << "condition:\n";
            printExpr(*d.condition, indentLevel + 2);
            indent(indentLevel + 1);
            out_ << "then: " << d.thenBody.size() << " stmts\n";
            if (!d.elseBody.empty()) {
                indent(indentLevel + 1);
                out_ << "else: " << d.elseBody.size() << " stmts\n";
            }
            indent(indentLevel); out_ << "}\n";
            break;
        }
        case DeclKind::Guard: {
            out_ << "GuardDecl\n";
            break;
        }
        case DeclKind::Switch: {
            out_ << "SwitchDecl\n";
            break;
        }
        case DeclKind::ForIn: {
            out_ << "ForInDecl\n";
            break;
        }
        case DeclKind::While: {
            out_ << "WhileDecl\n";
            break;
        }
        case DeclKind::RepeatWhile: {
            out_ << "RepeatWhileDecl\n";
            break;
        }
        case DeclKind::DoCatch: {
            out_ << "DoCatchDecl\n";
            break;
        }
        case DeclKind::Throw: {
            out_ << "ThrowDecl\n";
            break;
        }
        case DeclKind::Select: {
            out_ << "SelectDecl\n";
            break;
        }
        case DeclKind::Unsafe: {
            out_ << "UnsafeDecl\n";
            break;
        }
        case DeclKind::Subscript: {
            out_ << "SubscriptDecl\n";
            break;
        }
        case DeclKind::Init: {
            out_ << "InitDecl\n";
            break;
        }
        case DeclKind::Deinit: {
            out_ << "DeinitDecl\n";
            break;
        }
        case DeclKind::EnumCase: {
            out_ << "EnumCaseDecl\n";
            break;
        }
        case DeclKind::AssociatedType: {
            out_ << "AssociatedTypeDecl\n";
            break;
        }
        case DeclKind::Attribute: {
            out_ << "AttributeDecl\n";
            break;
        }
    }
}

void ASTPrinter::printExpr(const Expr& expr, int indentLevel) {
    indent(indentLevel);
    switch (expr.exprKind) {
        case ExprKind::IntegerLiteral: {
            auto& e = static_cast<const IntegerLiteralExpr&>(expr);
            out_ << "IntLit(" << e.value << ")\n";
            break;
        }
        case ExprKind::FloatLiteral: {
            auto& e = static_cast<const FloatLiteralExpr&>(expr);
            out_ << "FloatLit(" << e.value << ")\n";
            break;
        }
        case ExprKind::StringLiteral: {
            auto& e = static_cast<const StringLiteralExpr&>(expr);
            out_ << "StrLit(\"" << e.value << "\")\n";
            break;
        }
        case ExprKind::CharLiteral: {
            auto& e = static_cast<const CharLiteralExpr&>(expr);
            out_ << "CharLit(" << e.value << ")\n";
            break;
        }
        case ExprKind::BoolLiteral: {
            auto& e = static_cast<const BoolLiteralExpr&>(expr);
            out_ << "BoolLit(" << (e.value ? "true" : "false") << ")\n";
            break;
        }
        case ExprKind::NilLiteral: {
            out_ << "NilLit\n";
            break;
        }
        case ExprKind::Identifier: {
            auto& e = static_cast<const IdentifierExpr&>(expr);
            out_ << "Ident(\"" << e.name << "\")\n";
            break;
        }
        case ExprKind::Binary: {
            auto& e = static_cast<const BinaryExpr&>(expr);
            out_ << "BinaryExpr\n";
            printExpr(*e.left, indentLevel + 1);
            printExpr(*e.right, indentLevel + 1);
            break;
        }
        case ExprKind::Unary: {
            auto& e = static_cast<const UnaryExpr&>(expr);
            out_ << "UnaryExpr\n";
            printExpr(*e.operand, indentLevel + 1);
            break;
        }
        case ExprKind::Call: {
            auto& e = static_cast<const CallExpr&>(expr);
            out_ << "CallExpr {\n";
            indent(indentLevel + 1); out_ << "callee:\n";
            printExpr(*e.callee, indentLevel + 2);
            indent(indentLevel + 1); out_ << "args: " << e.args.size() << "\n";
            indent(indentLevel); out_ << "}\n";
            break;
        }
        case ExprKind::MemberAccess: {
            auto& e = static_cast<const MemberAccessExpr&>(expr);
            out_ << "MemberAccess(\"." << e.member << "\")\n";
            if (e.base) printExpr(*e.base, indentLevel + 1);
            break;
        }
        case ExprKind::ArrayLiteral: {
            auto& e = static_cast<const ArrayLiteralExpr&>(expr);
            out_ << "ArrayLit[" << e.elements.size() << "]\n";
            break;
        }
        case ExprKind::DictLiteral: {
            auto& e = static_cast<const DictLiteralExpr&>(expr);
            out_ << "DictLit[" << e.entries.size() << "]\n";
            break;
        }
        case ExprKind::SetLiteral: {
            out_ << "SetLit\n";
            break;
        }
        case ExprKind::Tuple: {
            out_ << "TupleExpr\n";
            break;
        }
        case ExprKind::Closure: {
            out_ << "ClosureExpr\n";
            break;
        }
        case ExprKind::TypeCast: {
            out_ << "TypeCastExpr\n";
            break;
        }
        case ExprKind::TypeCheck: {
            out_ << "TypeCheckExpr\n";
            break;
        }
        case ExprKind::OptionalChain: {
            out_ << "OptionalChainExpr\n";
            break;
        }
        case ExprKind::ForceUnwrap: {
            out_ << "ForceUnwrapExpr\n";
            break;
        }
        case ExprKind::Assignment: {
            out_ << "AssignmentExpr\n";
            break;
        }
        case ExprKind::InOut: {
            out_ << "InOutExpr\n";
            break;
        }
        case ExprKind::Move: {
            out_ << "MoveExpr\n";
            break;
        }
        case ExprKind::Await: {
            out_ << "AwaitExpr\n";
            break;
        }
        case ExprKind::Try: {
            out_ << "TryExpr\n";
            break;
        }
        case ExprKind::If: {
            out_ << "IfExpr\n";
            break;
        }
        case ExprKind::Subscript: {
            out_ << "SubscriptExpr\n";
            break;
        }
        case ExprKind::Selector: {
            out_ << "SelectorExpr\n";
            break;
        }
        case ExprKind::SuperRef: {
            out_ << "SuperRef\n";
            break;
        }
        case ExprKind::SelfRef: {
            out_ << "SelfRef\n";
            break;
        }
        case ExprKind::InterpolatedString: {
            out_ << "InterpolatedStringExpr\n";
            break;
        }
        case ExprKind::Prefix: {
            out_ << "PrefixExpr\n";
            break;
        }
        case ExprKind::Postfix: {
            out_ << "PostfixExpr\n";
            break;
        }
    }
}

void ASTPrinter::printStmt(const Stmt& stmt, int indentLevel) {
    indent(indentLevel);
    switch (stmt.stmtKind) {
        case StmtKind::Expression: {
            auto& s = static_cast<const ExpressionStmt&>(stmt);
            out_ << "ExprStmt\n";
            if (s.expression) printExpr(*s.expression, indentLevel + 1);
            break;
        }
        case StmtKind::Return: {
            auto& s = static_cast<const ReturnStmt&>(stmt);
            out_ << "ReturnStmt\n";
            if (s.value) printExpr(*s.value, indentLevel + 1);
            break;
        }
        case StmtKind::Break: {
            out_ << "BreakStmt\n";
            break;
        }
        case StmtKind::Continue: {
            out_ << "ContinueStmt\n";
            break;
        }
        case StmtKind::Fallthrough: {
            out_ << "FallthroughStmt\n";
            break;
        }
        case StmtKind::Defer: {
            out_ << "DeferStmt\n";
            break;
        }
        case StmtKind::VariableDecl: {
            out_ << "VarDecl (inline)\n";
            break;
        }
        case StmtKind::Compound: {
            auto& s = static_cast<const CompoundStmt&>(stmt);
            out_ << "CompoundStmt {\n";
            for (const auto& sub : s.statements) printStmt(*sub, indentLevel + 1);
            indent(indentLevel); out_ << "}\n";
            break;
        }
    }
}

void ASTPrinter::printTypeRepr(const TypeRepr& type, int indentLevel) {
    indent(indentLevel);
    switch (type.typeReprKind) {
        case TypeReprKind::Named: {
            auto& t = static_cast<const NamedTypeRepr&>(type);
            out_ << "NamedType(\"" << t.name << "\")\n";
            break;
        }
        case TypeReprKind::Array: {
            out_ << "ArrayType\n";
            break;
        }
        case TypeReprKind::Dictionary: {
            out_ << "DictType\n";
            break;
        }
        case TypeReprKind::Tuple: {
            out_ << "TupleType\n";
            break;
        }
        case TypeReprKind::Optional: {
            out_ << "OptionalType\n";
            break;
        }
        case TypeReprKind::Function: {
            out_ << "FunctionType\n";
            break;
        }
        case TypeReprKind::Composition: {
            out_ << "CompositionType\n";
            break;
        }
        case TypeReprKind::Opaque: {
            out_ << "OpaqueType(some)\n";
            break;
        }
        case TypeReprKind::Existential: {
            out_ << "ExistentialType(any)\n";
            break;
        }
        case TypeReprKind::Owned: {
            out_ << "OwnedType\n";
            break;
        }
        case TypeReprKind::Self: {
            out_ << "SelfType\n";
            break;
        }
        case TypeReprKind::Inferred: {
            out_ << "InferredType(_)\n";
            break;
        }
    }
}

void ASTPrinter::printPattern(const Pattern& pattern, int indentLevel) {
    indent(indentLevel);
    switch (pattern.patternKind) {
        case PatternKind::Identifier: {
            auto& p = static_cast<const IdentifierPattern&>(pattern);
            out_ << "IdentPattern(\"" << p.name << "\")\n";
            break;
        }
        case PatternKind::Wildcard: {
            out_ << "WildcardPattern\n";
            break;
        }
        case PatternKind::Tuple: {
            out_ << "TuplePattern\n";
            break;
        }
        case PatternKind::EnumCase: {
            out_ << "EnumCasePattern\n";
            break;
        }
        case PatternKind::IsType: {
            out_ << "IsTypePattern\n";
            break;
        }
        case PatternKind::AsType: {
            out_ << "AsTypePattern\n";
            break;
        }
        case PatternKind::Optional: {
            out_ << "OptionalPattern\n";
            break;
        }
        case PatternKind::Expression: {
            out_ << "ExprPattern\n";
            break;
        }
    }
}

} // namespace suki
