#include "compiler/ast/AST.h"
#include "compiler/lexer/Lexer.h"
#include "compiler/parser/Parser.h"
#include <fstream>
#include <iostream>
#include <string>

using namespace suki;

static std::string readFile(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) { std::cerr << "cannot open " << p << "\n"; std::exit(2); }
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

static const char* kindName(NodeKind k);
static int gDepth = 0;

static void indent() { for (int i = 0; i < gDepth; ++i) std::cout << "  "; }

static void dumpType(Node* n);
static void dumpExpr(Node* n);
static void dumpStmt(Node* n);
static void dumpDecl(Node* n);

static void dumpNode(Node* n) {
    if (!n) { indent(); std::cout << "(null)\n"; return; }
    switch (n->kind) {
        case NodeKind::NamedType: indent(); std::cout << "NamedType " << static_cast<NamedType*>(n)->name << "\n"; break;
        case NodeKind::OptionalType: indent(); std::cout << "OptionalType\n"; gDepth++; dumpType(static_cast<OptionalType*>(n)->wrapped.get()); gDepth--; break;
        case NodeKind::ArrayType: indent(); std::cout << "ArrayType\n"; gDepth++; dumpType(static_cast<ArrayType*>(n)->element.get()); gDepth--; break;
        case NodeKind::DictType: indent(); std::cout << "DictType\n"; gDepth++; dumpType(static_cast<DictType*>(n)->key.get()); dumpType(static_cast<DictType*>(n)->value.get()); gDepth--; break;
        case NodeKind::TupleType: indent(); std::cout << "TupleType(" << static_cast<TupleType*>(n)->elements.size() << ")\n"; break;
        case NodeKind::FuncType: indent(); std::cout << "FuncType\n"; break;
        case NodeKind::RefType: indent(); std::cout << "RefType " << static_cast<RefType*>(n)->refKind << "\n"; gDepth++; dumpType(static_cast<RefType*>(n)->pointee.get()); gDepth--; break;
        case NodeKind::InoutType: indent(); std::cout << "InoutType\n"; gDepth++; dumpType(static_cast<InoutType*>(n)->pointee.get()); gDepth--; break;
        case NodeKind::NeverType: indent(); std::cout << "NeverType\n"; break;
        case NodeKind::PlaceholderType: indent(); std::cout << "PlaceholderType\n"; break;
        case NodeKind::MetatypeType: indent(); std::cout << "MetatypeType\n"; break;
        default: dumpExpr(n); break;
    }
}
static void dumpType(Node* n) { dumpNode(n); }

static void dumpExpr(Node* n) {
    if (!n) { indent(); std::cout << "(null expr)\n"; return; }
    switch (n->kind) {
        case NodeKind::IdentExpr: indent(); std::cout << "Ident " << static_cast<IdentExpr*>(n)->name << "\n"; break;
        case NodeKind::IntLitExpr: indent(); std::cout << "Int " << static_cast<IntLitExpr*>(n)->value << "\n"; break;
        case NodeKind::FloatLitExpr: indent(); std::cout << "Float " << static_cast<FloatLitExpr*>(n)->value << "\n"; break;
        case NodeKind::StrLitExpr: indent(); std::cout << "String (segs=" << static_cast<StrLitExpr*>(n)->segments.size() << ", exprs=" << static_cast<StrLitExpr*>(n)->expressions.size() << ")\n"; break;
        case NodeKind::CharLitExpr: indent(); std::cout << "Char " << static_cast<CharLitExpr*>(n)->value << "\n"; break;
        case NodeKind::BoolLitExpr: indent(); std::cout << "Bool " << static_cast<BoolLitExpr*>(n)->value << "\n"; break;
        case NodeKind::BinaryExpr: { auto* b = static_cast<BinaryExpr*>(n); indent(); std::cout << "Binary " << punctToString(b->op) << "\n"; gDepth++; dumpExpr(b->lhs.get()); dumpExpr(b->rhs.get()); gDepth--; break; }
        case NodeKind::UnaryExpr: { auto* u = static_cast<UnaryExpr*>(n); indent(); std::cout << "Unary " << punctToString(u->op) << (u->isPostfix ? " (post)" : "") << "\n"; gDepth++; dumpExpr(u->operand.get()); gDepth--; break; }
        case NodeKind::CallExpr: { auto* c = static_cast<CallExpr*>(n); indent(); std::cout << "Call (args=" << c->arguments.size() << ")\n"; gDepth++; dumpExpr(c->callee.get()); for (auto& a : c->arguments) dumpExpr(a.get()); gDepth--; break; }
        case NodeKind::MemberExpr: { auto* m = static_cast<MemberExpr*>(n); indent(); std::cout << "Member ." << m->member << (m->optionalChain ? "?" : "") << "\n"; gDepth++; dumpExpr(m->base.get()); gDepth--; break; }
        case NodeKind::SubscriptExpr: { auto* s = static_cast<SubscriptExpr*>(n); indent(); std::cout << "Subscript (idx=" << s->indices.size() << ")\n"; gDepth++; dumpExpr(s->base.get()); for (auto& i : s->indices) dumpExpr(i.get()); gDepth--; break; }
        case NodeKind::OptionalChainExpr: indent(); std::cout << "OptionalChain\n"; gDepth++; dumpExpr(static_cast<OptionalChainExpr*>(n)->expr.get()); gDepth--; break;
        case NodeKind::ForceUnwrapExpr: indent(); std::cout << "ForceUnwrap\n"; gDepth++; dumpExpr(static_cast<ForceUnwrapExpr*>(n)->expr.get()); gDepth--; break;
        case NodeKind::TupleExpr: indent(); std::cout << "Tuple (" << static_cast<TupleExpr*>(n)->elements.size() << ")\n"; gDepth++; for (auto& e : static_cast<TupleExpr*>(n)->elements) dumpExpr(e.get()); gDepth--; break;
        case NodeKind::ArrayLitExpr: indent(); std::cout << "ArrayLit (" << static_cast<ArrayLitExpr*>(n)->elements.size() << ")\n"; break;
        case NodeKind::DictLitExpr: indent(); std::cout << "DictLit (" << static_cast<DictLitExpr*>(n)->keys.size() << ")\n"; break;
        case NodeKind::ClosureExpr: indent(); std::cout << "Closure (params=" << static_cast<ClosureExpr*>(n)->params.size() << ", stmts=" << static_cast<ClosureExpr*>(n)->body.size() << ")\n"; break;
        case NodeKind::ParenExpr: indent(); std::cout << "Paren\n"; gDepth++; dumpExpr(static_cast<ParenExpr*>(n)->expr.get()); gDepth--; break;
        case NodeKind::AsExpr: indent(); std::cout << "As\n"; gDepth++; dumpExpr(static_cast<AsExpr*>(n)->expr.get()); dumpType(static_cast<AsExpr*>(n)->type.get()); gDepth--; break;
        case NodeKind::IsExpr: indent(); std::cout << "Is\n"; gDepth++; dumpExpr(static_cast<IsExpr*>(n)->expr.get()); dumpType(static_cast<IsExpr*>(n)->type.get()); gDepth--; break;
        case NodeKind::AssignmentExpr: { auto* a = static_cast<AssignmentExpr*>(n); indent(); std::cout << (a->isCompound ? "Assign(compound)" : "Assign") << "\n"; gDepth++; dumpExpr(a->lhs.get()); dumpExpr(a->rhs.get()); gDepth--; break; }
        case NodeKind::RangeExpr: { auto* r = static_cast<RangeExpr*>(n); indent(); std::cout << "Range " << (r->halfOpen ? "..<" : "...") << "\n"; gDepth++; dumpExpr(r->lower.get()); dumpExpr(r->upper.get()); gDepth--; break; }
        case NodeKind::NilLitExpr: indent(); std::cout << "Nil\n"; break;
        case NodeKind::GenericExpr: { auto* g = static_cast<GenericExpr*>(n); indent(); std::cout << "GenericSpecialization\n"; gDepth++; dumpExpr(g->base.get()); for (auto& a : g->args) dumpType(a.get()); gDepth--; break; }
        case NodeKind::MoveExpr: indent(); std::cout << "Move\n"; gDepth++; dumpExpr(static_cast<MoveExpr*>(n)->operand.get()); gDepth--; break;
        case NodeKind::IfExpr: { auto* e = static_cast<IfExpr*>(n); indent(); std::cout << "IfExpr\n"; gDepth++; dumpExpr(e->condition.get()); indent(); std::cout << "then:\n"; for (auto& s : e->thenBody) dumpStmt(s.get()); if (e->elseBranch) { indent(); std::cout << "else:\n"; dumpNode(e->elseBranch.get()); } gDepth--; break; }
        default: indent(); std::cout << "Expr(" << kindName(n->kind) << ")\n"; break;
    }
}

static void dumpStmt(Node* n) {
    if (!n) { indent(); std::cout << "(null stmt)\n"; return; }
    switch (n->kind) {
        case NodeKind::BlockStmt: indent(); std::cout << "Block\n"; gDepth++; for (auto& s : static_cast<BlockStmt*>(n)->statements) dumpStmt(s.get()); gDepth--; break;
        case NodeKind::ExprStmt: indent(); std::cout << "ExprStmt\n"; gDepth++; dumpExpr(static_cast<ExprStmt*>(n)->expr.get()); gDepth--; break;
        case NodeKind::ReturnStmt: indent(); std::cout << "Return\n"; gDepth++; dumpExpr(static_cast<ReturnStmt*>(n)->value.get()); gDepth--; break;
        case NodeKind::IfStmt: { auto* i = static_cast<IfStmt*>(n); indent(); std::cout << "If\n"; gDepth++; dumpExpr(i->condition.get()); indent(); std::cout << "then:\n"; for (auto& s : i->thenBody) dumpStmt(s.get()); if (i->elseBranch) { indent(); std::cout << "else:\n"; dumpStmt(i->elseBranch.get()); } gDepth--; break; }
        case NodeKind::GuardStmt: indent(); std::cout << "Guard\n"; break;
        case NodeKind::WhileStmt: { auto* w = static_cast<WhileStmt*>(n); indent(); std::cout << "While\n"; gDepth++; dumpExpr(w->condition.get()); for (auto& s : w->body) dumpStmt(s.get()); gDepth--; break; }
        case NodeKind::RepeatWhileStmt: indent(); std::cout << "RepeatWhile\n"; break;
        case NodeKind::ForInStmt: { auto* f = static_cast<ForInStmt*>(n); indent(); std::cout << "ForIn\n"; gDepth++; dumpStmt(f->pattern.get()); dumpExpr(f->sequence.get()); for (auto& s : f->body) dumpStmt(s.get()); gDepth--; break; }
        case NodeKind::SwitchStmt: indent(); std::cout << "Switch (cases=" << static_cast<SwitchStmt*>(n)->cases.size() << ")\n"; break;
        case NodeKind::BreakStmt: indent(); std::cout << "Break\n"; break;
        case NodeKind::ContinueStmt: indent(); std::cout << "Continue\n"; break;
        case NodeKind::DeferStmt: indent(); std::cout << "Defer\n"; break;
        case NodeKind::UnsafeStmt: indent(); std::cout << "Unsafe\n"; gDepth++; for (auto& s : static_cast<UnsafeStmt*>(n)->body) dumpStmt(s.get()); gDepth--; break;
        case NodeKind::DoStmt: indent(); std::cout << "Do (catches=" << static_cast<DoStmt*>(n)->catches.size() << ")\n"; break;
        case NodeKind::ThrowStmt: indent(); std::cout << "Throw\n"; gDepth++; dumpExpr(static_cast<ThrowStmt*>(n)->value.get()); gDepth--; break;
        case NodeKind::VarDecl: { auto* v = static_cast<VarDecl*>(n); indent(); std::cout << (v->isLet ? "Let " : "Var ") << v->name; if (v->type) { std::cout << " : "; } std::cout << "\n"; gDepth++; if (v->type) dumpType(v->type.get()); if (v->initializer) dumpExpr(v->initializer.get()); gDepth--; break; }
        default: indent(); std::cout << "Stmt(" << kindName(n->kind) << ")\n"; break;
    }
}

static void dumpDecl(Node* n) {
    if (!n) { indent(); std::cout << "(null decl)\n"; return; }
    switch (n->kind) {
        case NodeKind::ModuleDecl: indent(); std::cout << "Module " << static_cast<ModuleDecl*>(n)->name << "\n"; break;
        case NodeKind::ImportDecl: indent(); std::cout << "Import " << static_cast<ImportDecl*>(n)->moduleName << "\n"; break;
        case NodeKind::FunctionDecl: { auto* f = static_cast<FunctionDecl*>(n); indent(); std::cout << "Func " << f->name << " (params=" << f->params.size() << ", foreign=" << f->isForeign << ")\n"; gDepth++; if (f->returnType) dumpType(f->returnType.get()); for (auto& s : f->body) dumpStmt(s.get()); gDepth--; break; }
        case NodeKind::StructDecl: case NodeKind::EnumDecl: case NodeKind::ClassDecl:
        case NodeKind::ActorDecl:
        case NodeKind::ProtocolDecl: case NodeKind::ExtensionDecl: {
            auto* t = static_cast<TypeDecl*>(n);
            const char* tn = (n->kind==NodeKind::StructDecl)?"Struct":(n->kind==NodeKind::EnumDecl)?"Enum":(n->kind==NodeKind::ClassDecl)?"Class":(n->kind==NodeKind::ActorDecl)?"Actor":(n->kind==NodeKind::ProtocolDecl)?"Protocol":"Extension";
            indent(); std::cout << tn << " " << t->name << " (members=" << t->members.size() << ")\n"; gDepth++; for (auto& m : t->members) dumpDecl(m.get()); gDepth--; break; }
        case NodeKind::InitDecl: indent(); std::cout << "Init\n"; gDepth++; for (auto& s : static_cast<InitDecl*>(n)->body) dumpStmt(s.get()); gDepth--; break;
        case NodeKind::DeinitDecl: indent(); std::cout << "Deinit\n"; gDepth++; for (auto& s : static_cast<DeinitDecl*>(n)->body) dumpStmt(s.get()); gDepth--; break;
        case NodeKind::TypealiasDecl: indent(); std::cout << "Typealias " << static_cast<TypealiasDecl*>(n)->name << "\n"; break;
        case NodeKind::EnumCaseDecl: indent(); std::cout << "EnumCase " << static_cast<EnumCaseDecl*>(n)->name << "\n"; break;
        case NodeKind::VarDecl: dumpStmt(n); break;
        default: indent(); std::cout << "Decl(" << kindName(n->kind) << ")\n"; break;
    }
}

static const char* kindName(NodeKind k) {
    switch (k) {
        case NodeKind::ModuleDecl: return "ModuleDecl";
        case NodeKind::ImportDecl: return "ImportDecl";
        case NodeKind::FunctionDecl: return "FunctionDecl";
        case NodeKind::StructDecl: return "StructDecl";
        case NodeKind::EnumDecl: return "EnumDecl";
        case NodeKind::ClassDecl: return "ClassDecl";
        case NodeKind::ProtocolDecl: return "ProtocolDecl";
        case NodeKind::ExtensionDecl: return "ExtensionDecl";
        case NodeKind::InitDecl: return "InitDecl";
        case NodeKind::DeinitDecl: return "DeinitDecl";
        case NodeKind::SubscriptDecl: return "SubscriptDecl";
        case NodeKind::VarDecl: return "VarDecl";
        case NodeKind::TypealiasDecl: return "TypealiasDecl";
        case NodeKind::EnumCaseDecl: return "EnumCaseDecl";
        case NodeKind::AssociatedTypeDecl: return "AssociatedTypeDecl";
        case NodeKind::MacroDecl: return "MacroDecl";
        case NodeKind::BlockStmt: return "BlockStmt";
        case NodeKind::ExprStmt: return "ExprStmt";
        case NodeKind::ReturnStmt: return "ReturnStmt";
        case NodeKind::IfStmt: return "IfStmt";
        case NodeKind::GuardStmt: return "GuardStmt";
        case NodeKind::WhileStmt: return "WhileStmt";
        case NodeKind::RepeatWhileStmt: return "RepeatWhileStmt";
        case NodeKind::ForInStmt: return "ForInStmt";
        case NodeKind::SwitchStmt: return "SwitchStmt";
        case NodeKind::CaseClause: return "CaseClause";
        case NodeKind::BreakStmt: return "BreakStmt";
        case NodeKind::ContinueStmt: return "ContinueStmt";
        case NodeKind::DeferStmt: return "DeferStmt";
        case NodeKind::DoStmt: return "DoStmt";
        case NodeKind::CatchClause: return "CatchClause";
        case NodeKind::ThrowStmt: return "ThrowStmt";
        case NodeKind::IdentExpr: return "IdentExpr";
        case NodeKind::IntLitExpr: return "IntLitExpr";
        case NodeKind::FloatLitExpr: return "FloatLitExpr";
        case NodeKind::StrLitExpr: return "StrLitExpr";
        case NodeKind::CharLitExpr: return "CharLitExpr";
        case NodeKind::BoolLitExpr: return "BoolLitExpr";
        case NodeKind::BinaryExpr: return "BinaryExpr";
        case NodeKind::UnaryExpr: return "UnaryExpr";
        case NodeKind::CallExpr: return "CallExpr";
        case NodeKind::MemberExpr: return "MemberExpr";
        case NodeKind::SubscriptExpr: return "SubscriptExpr";
        case NodeKind::OptionalChainExpr: return "OptionalChainExpr";
        case NodeKind::ForceUnwrapExpr: return "ForceUnwrapExpr";
        case NodeKind::TupleExpr: return "TupleExpr";
        case NodeKind::ArrayLitExpr: return "ArrayLitExpr";
        case NodeKind::DictLitExpr: return "DictLitExpr";
        case NodeKind::ClosureExpr: return "ClosureExpr";
        case NodeKind::ParenExpr: return "ParenExpr";
        case NodeKind::AsExpr: return "AsExpr";
        case NodeKind::IsExpr: return "IsExpr";
        case NodeKind::AssignmentExpr: return "AssignmentExpr";
        case NodeKind::RangeExpr: return "RangeExpr";
        case NodeKind::NilLitExpr: return "NilLitExpr";
        case NodeKind::ActorDecl: return "ActorDecl";
        case NodeKind::UnsafeStmt: return "UnsafeStmt";
        case NodeKind::GenericExpr: return "GenericExpr";
        case NodeKind::MoveExpr: return "MoveExpr";
        case NodeKind::IfExpr: return "IfExpr";
        case NodeKind::NamedType: return "NamedType";
        case NodeKind::OptionalType: return "OptionalType";
        case NodeKind::ArrayType: return "ArrayType";
        case NodeKind::DictType: return "DictType";
        case NodeKind::TupleType: return "TupleType";
        case NodeKind::FuncType: return "FuncType";
        case NodeKind::RefType: return "RefType";
        case NodeKind::InoutType: return "InoutType";
        case NodeKind::NeverType: return "NeverType";
        case NodeKind::PlaceholderType: return "PlaceholderType";
        case NodeKind::MetatypeType: return "MetatypeType";
    }
    return "?";
}

int main(int argc, char** argv) {
    if (argc < 2) { std::cerr << "usage: parse_verify <file.suki>\n"; return 2; }
    std::string src = readFile(argv[1]);
    DiagnosticEngine diags;
    Lexer lexer(src, diags);
    auto toks = lexer.tokenizeAll();
    Parser parser(std::move(toks), diags);
    auto decls = parser.parseModule();
    for (auto& d : decls) dumpDecl(d.get());
    std::cout << "--- diagnostics ---\n";
    diags.emit();
    return diags.hasErrors() ? 1 : 0;
}
