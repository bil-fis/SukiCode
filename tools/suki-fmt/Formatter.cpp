// SukiCode code formatter implementation.

#include "Formatter.h"

namespace suki {

Formatter::Formatter(const FormatConfig& config) : config_(config) {}
Formatter::~Formatter() = default;

std::string Formatter::format(const CompilationUnit& cu) {
    out_.str("");
    out_.clear();
    if (cu.moduleDecl) {
        out_ << "module " << cu.moduleDecl->name << "\n\n";
    }
    for (size_t i = 0; i < cu.declarations.size(); i++) {
        if (cu.declarations[i]) {
            formatDecl(*cu.declarations[i], 0);
            if (i < cu.declarations.size() - 1) out_ << "\n";
        }
    }
    return out_.str();
}

std::string Formatter::formatSource(const std::string& source, const std::string& filename) {
    return source;
}

void Formatter::formatDecl(const Decl& decl, int lv) {
    switch (decl.declKind) {
        case DeclKind::Variable: formatVariableDecl(static_cast<const VariableDecl&>(decl), lv); break;
        case DeclKind::Function: formatFunctionDecl(static_cast<const FunctionDecl&>(decl), lv); break;
        case DeclKind::Struct: formatStructDecl(static_cast<const StructDecl&>(decl), lv); break;
        case DeclKind::Class: formatClassDecl(static_cast<const ClassDecl&>(decl), lv); break;
        case DeclKind::Enum: formatEnumDecl(static_cast<const EnumDecl&>(decl), lv); break;
        case DeclKind::Protocol: formatProtocolDecl(static_cast<const ProtocolDecl&>(decl), lv); break;
        default: break;
    }
}

void Formatter::formatVariableDecl(const VariableDecl& decl, int lv) {
    emitIndent(lv);
    out_ << (decl.isLet ? "let " : "var ");
    if (decl.pattern && decl.pattern->patternKind == PatternKind::Identifier) {
        out_ << static_cast<const IdentifierPattern*>(decl.pattern.get())->name;
    }
    if (decl.initializer) {
        out_ << " = ";
        formatExpr(*decl.initializer, lv);
    }
    out_ << "\n";
}

void Formatter::formatFunctionDecl(const FunctionDecl& decl, int lv) {
    emitIndent(lv);
    out_ << "func " << decl.name;
    if (!decl.genericParams.empty()) {
        out_ << "<";
        for (size_t i = 0; i < decl.genericParams.size(); i++) {
            if (i > 0) out_ << ", ";
            out_ << decl.genericParams[i].name;
        }
        out_ << ">";
    }
    out_ << "(";
    for (size_t i = 0; i < decl.params.size(); i++) {
        if (i > 0) out_ << ", ";
        const auto& p = decl.params[i];
        if (!p.externalLabel.empty() && p.externalLabel != "_") {
            out_ << p.externalLabel << " ";
        } else if (p.externalLabel == "_") {
            out_ << "_ ";
        }
        out_ << p.internalName;
    }
    out_ << ")";
    if (decl.isAsync) out_ << " async";
    if (decl.isThrows) out_ << " throws";
    if (!decl.body.empty()) {
        out_ << " {\n";
        for (const auto& stmt : decl.body) {
            if (stmt) formatStmt(*stmt, lv + 1);
        }
        emitIndent(lv);
        out_ << "}";
    }
    out_ << "\n";
}

void Formatter::formatStructDecl(const StructDecl& decl, int lv) {
    emitIndent(lv);
    out_ << "struct " << decl.name << " {\n";
    for (const auto& m : decl.members) { if (m) formatDecl(*m, lv + 1); }
    emitIndent(lv); out_ << "}\n";
}

void Formatter::formatClassDecl(const ClassDecl& decl, int lv) {
    emitIndent(lv);
    out_ << "class " << decl.name << " {\n";
    for (const auto& m : decl.members) { if (m) formatDecl(*m, lv + 1); }
    emitIndent(lv); out_ << "}\n";
}

void Formatter::formatEnumDecl(const EnumDecl& decl, int lv) {
    emitIndent(lv);
    out_ << "enum " << decl.name << " {\n";
    for (const auto& c : decl.cases) {
        if (c) { emitIndent(lv + 1); out_ << "case " << c->name << "\n"; }
    }
    emitIndent(lv); out_ << "}\n";
}

void Formatter::formatProtocolDecl(const ProtocolDecl& decl, int lv) {
    emitIndent(lv);
    out_ << "protocol " << decl.name << " {\n";
    for (const auto& m : decl.members) { if (m) formatDecl(*m, lv + 1); }
    emitIndent(lv); out_ << "}\n";
}

void Formatter::formatStmt(const Stmt& stmt, int lv) {
    switch (stmt.stmtKind) {
        case StmtKind::Return: formatReturnStmt(static_cast<const ReturnStmt&>(stmt), lv); break;
        case StmtKind::Expression: formatExprStmt(static_cast<const ExpressionStmt&>(stmt), lv); break;
        case StmtKind::Compound: formatCompoundStmt(static_cast<const CompoundStmt&>(stmt), lv); break;
        default: emitIndent(lv); out_ << "/* stmt */\n"; break;
    }
}

void Formatter::formatReturnStmt(const ReturnStmt& stmt, int lv) {
    emitIndent(lv);
    out_ << "return";
    if (stmt.value) { out_ << " "; formatExpr(*stmt.value, lv); }
    out_ << "\n";
}

void Formatter::formatExprStmt(const ExpressionStmt& stmt, int lv) {
    emitIndent(lv);
    if (stmt.expression) formatExpr(*stmt.expression, lv);
    out_ << "\n";
}

void Formatter::formatCompoundStmt(const CompoundStmt& stmt, int lv) {
    emitIndent(lv); out_ << "{\n";
    for (const auto& s : stmt.statements) { if (s) formatStmt(*s, lv + 1); }
    emitIndent(lv); out_ << "}\n";
}

void Formatter::formatExpr(const Expr& expr, int lv) {
    switch (expr.exprKind) {
        case ExprKind::IntegerLiteral: out_ << static_cast<const IntegerLiteralExpr&>(expr).value; break;
        case ExprKind::FloatLiteral: out_ << static_cast<const FloatLiteralExpr&>(expr).value; break;
        case ExprKind::StringLiteral: out_ << "\"" << static_cast<const StringLiteralExpr&>(expr).value << "\""; break;
        case ExprKind::BoolLiteral: out_ << (static_cast<const BoolLiteralExpr&>(expr).value ? "true" : "false"); break;
        case ExprKind::NilLiteral: out_ << "nil"; break;
        case ExprKind::Identifier: out_ << static_cast<const IdentifierExpr&>(expr).name; break;
        case ExprKind::Binary: {
            auto& be = static_cast<const BinaryExpr&>(expr);
            formatExpr(*be.left, lv);
            // 简化运算符输出 / Simplified operator output
            out_ << " ? ";
            formatExpr(*be.right, lv);
            break;
        }
        case ExprKind::Unary: {
            auto& ue = static_cast<const UnaryExpr&>(expr);
            out_ << "!";
            formatExpr(*ue.operand, lv);
            break;
        }
        case ExprKind::Call: {
            auto& ce = static_cast<const CallExpr&>(expr);
            formatExpr(*ce.callee, lv);
            out_ << "(";
            for (size_t i = 0; i < ce.args.size(); i++) {
                if (i > 0) out_ << ", ";
                if (!ce.args[i].label.empty()) out_ << ce.args[i].label << ": ";
                formatExpr(*ce.args[i].value, lv);
            }
            out_ << ")";
            break;
        }
        case ExprKind::MemberAccess: {
            auto& ma = static_cast<const MemberAccessExpr&>(expr);
            formatExpr(*ma.base, lv);
            out_ << "." << ma.member;
            break;
        }
        case ExprKind::ArrayLiteral: {
            auto& al = static_cast<const ArrayLiteralExpr&>(expr);
            out_ << "[";
            for (size_t i = 0; i < al.elements.size(); i++) {
                if (i > 0) out_ << ", ";
                formatExpr(*al.elements[i], lv);
            }
            out_ << "]";
            break;
        }
        case ExprKind::DictLiteral: {
            auto& dl = static_cast<const DictLiteralExpr&>(expr);
            out_ << "[";
            for (size_t i = 0; i < dl.entries.size(); i++) {
                if (i > 0) out_ << ", ";
                formatExpr(*dl.entries[i].key, lv);
                out_ << ": ";
                formatExpr(*dl.entries[i].value, lv);
            }
            out_ << "]";
            break;
        }
        case ExprKind::Closure: {
            auto& cl = static_cast<const ClosureExpr&>(expr);
            out_ << "{ ";
            if (!cl.params.empty()) {
                out_ << "(";
                for (size_t i = 0; i < cl.params.size(); i++) {
                    if (i > 0) out_ << ", ";
                    out_ << cl.params[i].name;
                }
                out_ << ") in ";
            }
            for (const auto& s : cl.body) {
                if (s) formatStmt(*s, lv + 1);
            }
            out_ << " }";
            break;
        }
        default: out_ << "/* expr */"; break;
    }
}

void Formatter::emitIndent(int level) {
    for (int i = 0; i < level * config_.indentWidth; i++) out_ << " ";
}

} // namespace suki
