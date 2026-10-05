// SukiCode compiler driver — `sukic`
//
// Frontend driver for the SukiCode toolchain. This entry point wires the
// pipeline stages together and exposes them through a small command-line
// interface. Later stages (macro expansion, semantic analysis, LLVM IR
// generation) are plugged in behind the same commands as they land.
//
//   sukic lex    <file>...            dump the token stream
//   sukic parse  <file>... [--dump-ast]   parse to AST, report diagnostics
//   sukic version                     print version information
//   sukic help                        print this help
//
// The driver never throws; all failures are surfaced as diagnostics and a
// non-zero process exit code.

#include "compiler/ast/AST.h"
#include "compiler/diag/DiagnosticEngine.h"
#include "compiler/lexer/Lexer.h"
#include "compiler/parser/Parser.h"
#include "compiler/sema/Sema.h"
#include "compiler/codegen/IRGenerator.h"
#include "compiler/codegen/TargetInfo.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace suki;

static const char* kVersion = "0.1.0";

static bool readFile(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out.assign((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    return true;
}

// ─── Token kind names ───────────────────────────────────────────────────────
static const char* tokenKindName(TokenKind k) {
    switch (k) {
        case TokenKind::TK_EOF: return "EOF";
        case TokenKind::TK_Error: return "Error";
        case TokenKind::TK_Identifier: return "Ident";
        case TokenKind::TK_IntLiteral: return "Int";
        case TokenKind::TK_FloatLiteral: return "Float";
        case TokenKind::TK_StringLiteral: return "String";
        case TokenKind::TK_StringFragment: return "StringFrag";
        case TokenKind::TK_StringEnd: return "StringEnd";
        case TokenKind::TK_CharLiteral: return "Char";
        case TokenKind::TK_Keyword: return "Keyword";
        case TokenKind::TK_Punctuator: return "Punct";
    }
    return "?";
}

// ─── AST printer ───────────────────────────────────────────────────────────
namespace {

struct AstPrinter {
    FILE* out  = stdout;
    int depth = 0;
    void indent() { for (int i = 0; i < depth; ++i) fputs("  ", out); }
    void line(const std::string& s) { indent(); fputs(s.c_str(), out); fputc('\n', out); }
};

const char* kindName(NodeKind k);

void dumpNode(AstPrinter& p, Node* n);

void dumpParams(AstPrinter& p, const std::vector<Param>& params) {
    for (const auto& prm : params) {
        std::string s = "Param " + (prm.externalName.empty() ? std::string("_") : prm.externalName);
        if (prm.externalName != prm.internalName) s += " (internal " + prm.internalName + ")";
        if (prm.isVariadic) s += "...";
        p.line(s);
        ++p.depth;
        if (prm.type) dumpNode(p, prm.type.get());
        if (prm.defaultValue) dumpNode(p, prm.defaultValue.get());
        --p.depth;
    }
}

void dumpNode(AstPrinter& p, Node* n) {
    if (!n) { p.line("(null)"); return; }
    switch (n->kind) {
        // ── Declarations ────────────────────────────────────────────────────
        case NodeKind::ModuleDecl:
            p.line("Module " + static_cast<ModuleDecl*>(n)->name); break;
        case NodeKind::ImportDecl:
            p.line("Import " + static_cast<ImportDecl*>(n)->moduleName); break;
        case NodeKind::FunctionDecl: {
            auto* f = static_cast<FunctionDecl*>(n);
            std::string s = "Func " + f->name + " (params=" + std::to_string(f->params.size());
            if (f->isAsync) s += ", async";
            if (f->isThrows) s += ", throws";
            if (f->isForeign) s += ", foreign";
            if (!f->genericParams.empty()) s += ", generic";
            s += ")";
            p.line(s);
            ++p.depth;
            if (f->returnType) dumpNode(p, f->returnType.get());
            dumpParams(p, f->params);
            for (auto& st : f->body) dumpNode(p, st.get());
            --p.depth;
            break;
        }
        case NodeKind::StructDecl: case NodeKind::EnumDecl: case NodeKind::ClassDecl:
        case NodeKind::ActorDecl: case NodeKind::ProtocolDecl: case NodeKind::ExtensionDecl: {
            auto* t = static_cast<TypeDecl*>(n);
            const char* kind = "Type";
            switch (n->kind) {
                case NodeKind::StructDecl: kind = "Struct"; break;
                case NodeKind::EnumDecl: kind = "Enum"; break;
                case NodeKind::ClassDecl: kind = "Class"; break;
                case NodeKind::ActorDecl: kind = "Actor"; break;
                case NodeKind::ProtocolDecl: kind = "Protocol"; break;
                case NodeKind::ExtensionDecl: kind = "Extension"; break;
                default: break;
            }
            p.line(std::string(kind) + " " + t->name + " (members=" +
                   std::to_string(t->members.size()) + ")");
            ++p.depth;
            for (auto& m : t->members) dumpNode(p, m.get());
            --p.depth;
            break;
        }
        case NodeKind::InitDecl: {
            auto* d = static_cast<InitDecl*>(n);
            p.line("Init");
            ++p.depth;
            dumpParams(p, d->params);
            for (auto& st : d->body) dumpNode(p, st.get());
            --p.depth;
            break;
        }
        case NodeKind::DeinitDecl: {
            auto* d = static_cast<DeinitDecl*>(n);
            p.line("Deinit");
            ++p.depth;
            for (auto& st : d->body) dumpNode(p, st.get());
            --p.depth;
            break;
        }
        case NodeKind::SubscriptDecl:
            p.line("Subscript"); break;
        case NodeKind::TypealiasDecl: {
            auto* t = static_cast<TypealiasDecl*>(n);
            p.line("Typealias " + t->name);
            ++p.depth;
            if (t->underlying) dumpNode(p, t->underlying.get());
            --p.depth;
            break;
        }
        case NodeKind::EnumCaseDecl:
            p.line("EnumCase " + static_cast<EnumCaseDecl*>(n)->name); break;
        case NodeKind::AssociatedTypeDecl:
            p.line("AssociatedType " + static_cast<AssociatedTypeDecl*>(n)->name); break;
        case NodeKind::MacroDecl:
            p.line("Macro " + static_cast<MacroDecl*>(n)->name); break;
        case NodeKind::VarDecl: {
            auto* v = static_cast<VarDecl*>(n);
            p.line(std::string(v->isLet ? "Let " : "Var ") + v->name +
                   (v->isWeak ? " (weak)" : ""));
            ++p.depth;
            if (v->type) dumpNode(p, v->type.get());
            if (v->initializer) dumpNode(p, v->initializer.get());
            // Accessors keep their kind, which matters for how they lower.
            for (auto& a : v->accessors) {
                if (!a || a->kind != NodeKind::AccessorDecl) continue;
                auto* ad = static_cast<AccessorDecl*>(a.get());
                const char* kind = ad->kind == AccessorDecl::Kind::Getter ? "get"
                            : ad->kind == AccessorDecl::Kind::Setter ? "set"
                            : ad->kind == AccessorDecl::Kind::WillSet ? "willSet"
                                                                      : "didSet";
                p.line(std::string(kind) +
                       (ad->valueParam.empty() ? "" : "(" + ad->valueParam + ")"));
                ++p.depth;
                for (auto& st : ad->body) dumpNode(p, st.get());
                --p.depth;
            }
            --p.depth;
            break;
        }

        // ── Statements ──────────────────────────────────────────────────────
        case NodeKind::BlockStmt: {
            auto* b = static_cast<BlockStmt*>(n);
            p.line("Block");
            ++p.depth;
            for (auto& st : b->statements) dumpNode(p, st.get());
            --p.depth;
            break;
        }
        case NodeKind::ExprStmt: {
            p.line("ExprStmt");
            ++p.depth;
            dumpNode(p, static_cast<ExprStmt*>(n)->expr.get());
            --p.depth;
            break;
        }
        case NodeKind::ReturnStmt: {
            p.line("Return");
            ++p.depth;
            dumpNode(p, static_cast<ReturnStmt*>(n)->value.get());
            --p.depth;
            break;
        }
        case NodeKind::IfStmt: {
            auto* s = static_cast<IfStmt*>(n);
            p.line("If");
            ++p.depth;
            dumpNode(p, s->condition.get());
            p.line("then:");
            for (auto& st : s->thenBody) dumpNode(p, st.get());
            if (s->elseBranch) { p.line("else:"); dumpNode(p, s->elseBranch.get()); }
            --p.depth;
            break;
        }
        case NodeKind::IfExpr: {
            auto* e = static_cast<IfExpr*>(n);
            p.line("IfExpr");
            ++p.depth;
            dumpNode(p, e->condition.get());
            p.line("then:");
            for (auto& st : e->thenBody) dumpNode(p, st.get());
            if (e->elseBranch) { p.line("else:"); dumpNode(p, e->elseBranch.get()); }
            --p.depth;
            break;
        }
        case NodeKind::GuardStmt: {
            auto* g = static_cast<GuardStmt*>(n);
            p.line("Guard");
            ++p.depth;
            dumpNode(p, g->condition.get());
            p.line("else:");
            for (auto& st : g->elseBody) dumpNode(p, st.get());
            --p.depth;
            break;
        }
        case NodeKind::WhileStmt: {
            auto* w = static_cast<WhileStmt*>(n);
            p.line("While");
            ++p.depth;
            dumpNode(p, w->condition.get());
            for (auto& st : w->body) dumpNode(p, st.get());
            --p.depth;
            break;
        }
        case NodeKind::RepeatWhileStmt: {
            auto* r = static_cast<RepeatWhileStmt*>(n);
            p.line("RepeatWhile");
            ++p.depth;
            for (auto& st : r->body) dumpNode(p, st.get());
            p.line("while:");
            dumpNode(p, r->condition.get());
            --p.depth;
            break;
        }
        case NodeKind::ForInStmt: {
            auto* f = static_cast<ForInStmt*>(n);
            p.line(std::string("ForIn") + (f->isAsync ? " (await)" : ""));
            ++p.depth;
            dumpNode(p, f->pattern.get());
            p.line("in:");
            dumpNode(p, f->sequence.get());
            for (auto& st : f->body) dumpNode(p, st.get());
            --p.depth;
            break;
        }
        case NodeKind::SwitchStmt: {
            auto* s = static_cast<SwitchStmt*>(n);
            p.line("Switch (cases=" + std::to_string(s->cases.size()) + ")");
            ++p.depth;
            if (s->subject) { p.line("subject:"); dumpNode(p, s->subject.get()); }
            for (auto& c : s->cases) dumpNode(p, c.get());
            --p.depth;
            break;
        }
        case NodeKind::CaseClause: {
            auto* c = static_cast<CaseClause*>(n);
            p.line(c->isDefault ? "Default" : "Case");
            ++p.depth;
            if (c->pattern) dumpNode(p, c->pattern.get());
            if (!c->bindings.empty()) {
                std::string s;
                for (size_t i = 0; i < c->bindings.size(); ++i) {
                    if (i) s += ", ";
                    s += c->bindings[i];
                }
                p.line("binds: " + s);
            }
            if (c->whereExpr) { p.line("where:"); dumpNode(p, c->whereExpr.get()); }
            for (auto& st : c->body) dumpNode(p, st.get());
            --p.depth;
            break;
        }
        case NodeKind::BreakStmt:   p.line("Break"); break;
        case NodeKind::ContinueStmt: p.line("Continue"); break;
        case NodeKind::DeferStmt: {
            p.line("Defer");
            ++p.depth;
            dumpNode(p, static_cast<DeferStmt*>(n)->body.get());
            --p.depth;
            break;
        }
        case NodeKind::DoStmt: {
            auto* d = static_cast<DoStmt*>(n);
            p.line("Do (catches=" + std::to_string(d->catches.size()) + ")");
            ++p.depth;
            for (auto& st : d->body) dumpNode(p, st.get());
            for (auto& c : d->catches) dumpNode(p, c.get());
            --p.depth;
            break;
        }
        case NodeKind::CatchClause: {
            auto* c = static_cast<CatchClause*>(n);
            p.line("Catch");
            ++p.depth;
            if (c->pattern) dumpNode(p, c->pattern.get());
            if (c->whereExpr) { p.line("where:"); dumpNode(p, c->whereExpr.get()); }
            for (auto& st : c->body) dumpNode(p, st.get());
            --p.depth;
            break;
        }
        case NodeKind::ThrowStmt: {
            p.line("Throw");
            ++p.depth;
            dumpNode(p, static_cast<ThrowStmt*>(n)->value.get());
            --p.depth;
            break;
        }
        case NodeKind::UnsafeStmt: {
            p.line("Unsafe");
            ++p.depth;
            for (auto& st : static_cast<UnsafeStmt*>(n)->body) dumpNode(p, st.get());
            --p.depth;
            break;
        }

        // ── Expressions ─────────────────────────────────────────────────────
        case NodeKind::IdentExpr:
            p.line("Ident " + static_cast<IdentExpr*>(n)->name); break;
        case NodeKind::IntLitExpr:
            p.line("Int " + static_cast<IntLitExpr*>(n)->value); break;
        case NodeKind::FloatLitExpr:
            p.line("Float " + static_cast<FloatLitExpr*>(n)->value); break;
        case NodeKind::StrLitExpr: {
            auto* s = static_cast<StrLitExpr*>(n);
            p.line("String (segs=" + std::to_string(s->segments.size()) +
                   ", exprs=" + std::to_string(s->expressions.size()) + ")");
            ++p.depth;
            for (auto& ex : s->expressions) dumpNode(p, ex.get());
            --p.depth;
            break;
        }
        case NodeKind::CharLitExpr:
            p.line("Char " + static_cast<CharLitExpr*>(n)->value); break;
        case NodeKind::BoolLitExpr:
            p.line(std::string("Bool ") +
                   (static_cast<BoolLitExpr*>(n)->value ? "true" : "false")); break;
        case NodeKind::NilLitExpr: p.line("Nil"); break;
        case NodeKind::BinaryExpr: {
            auto* b = static_cast<BinaryExpr*>(n);
            p.line(std::string("Binary ") + punctToString(b->op));
            ++p.depth;
            dumpNode(p, b->lhs.get());
            dumpNode(p, b->rhs.get());
            --p.depth;
            break;
        }
        case NodeKind::UnaryExpr: {
            auto* u = static_cast<UnaryExpr*>(n);
            p.line(std::string("Unary ") + punctToString(u->op) + (u->isPostfix ? " (post)" : ""));
            ++p.depth;
            dumpNode(p, u->operand.get());
            --p.depth;
            break;
        }
        case NodeKind::CallExpr: {
            auto* c = static_cast<CallExpr*>(n);
            p.line("Call (args=" + std::to_string(c->arguments.size()) +
                   (c->hasTrailingClosure ? ", trailing" : "") + ")");
            ++p.depth;
            dumpNode(p, c->callee.get());
            for (auto& a : c->arguments) dumpNode(p, a.get());
            --p.depth;
            break;
        }
        case NodeKind::MemberExpr: {
            auto* m = static_cast<MemberExpr*>(n);
            p.line("Member ." + m->member + (m->optionalChain ? "?" : ""));
            ++p.depth;
            dumpNode(p, m->base.get());
            --p.depth;
            break;
        }
        case NodeKind::SubscriptExpr: {
            auto* s = static_cast<SubscriptExpr*>(n);
            p.line("Subscript (idx=" + std::to_string(s->indices.size()) + ")");
            ++p.depth;
            dumpNode(p, s->base.get());
            for (auto& i : s->indices) dumpNode(p, i.get());
            --p.depth;
            break;
        }
        case NodeKind::OptionalChainExpr: {
            p.line("OptionalChain");
            ++p.depth;
            dumpNode(p, static_cast<OptionalChainExpr*>(n)->expr.get());
            --p.depth;
            break;
        }
        case NodeKind::ForceUnwrapExpr: {
            p.line("ForceUnwrap");
            ++p.depth;
            dumpNode(p, static_cast<ForceUnwrapExpr*>(n)->expr.get());
            --p.depth;
            break;
        }
        case NodeKind::TupleExpr: {
            auto* t = static_cast<TupleExpr*>(n);
            p.line("Tuple (" + std::to_string(t->elements.size()) + ")");
            ++p.depth;
            for (auto& e : t->elements) dumpNode(p, e.get());
            --p.depth;
            break;
        }
        case NodeKind::ArrayLitExpr: {
            auto* a = static_cast<ArrayLitExpr*>(n);
            p.line("ArrayLit (" + std::to_string(a->elements.size()) + ")");
            ++p.depth;
            for (auto& e : a->elements) dumpNode(p, e.get());
            --p.depth;
            break;
        }
        case NodeKind::DictLitExpr: {
            auto* d = static_cast<DictLitExpr*>(n);
            p.line("DictLit (" + std::to_string(d->keys.size()) + ")");
            ++p.depth;
            for (auto& e : d->keys) dumpNode(p, e.get());
            for (auto& e : d->values) dumpNode(p, e.get());
            --p.depth;
            break;
        }
        case NodeKind::ClosureExpr: {
            auto* c = static_cast<ClosureExpr*>(n);
            p.line("Closure (params=" + std::to_string(c->params.size()) +
                   ", stmts=" + std::to_string(c->body.size()) + ")");
            ++p.depth;
            dumpParams(p, c->params);
            for (auto& st : c->body) dumpNode(p, st.get());
            --p.depth;
            break;
        }
        case NodeKind::ParenExpr: {
            p.line("Paren");
            ++p.depth;
            dumpNode(p, static_cast<ParenExpr*>(n)->expr.get());
            --p.depth;
            break;
        }
        case NodeKind::AsExpr: {
            p.line("As");
            ++p.depth;
            dumpNode(p, static_cast<AsExpr*>(n)->expr.get());
            dumpNode(p, static_cast<AsExpr*>(n)->type.get());
            --p.depth;
            break;
        }
        case NodeKind::IsExpr: {
            p.line("Is");
            ++p.depth;
            dumpNode(p, static_cast<IsExpr*>(n)->expr.get());
            dumpNode(p, static_cast<IsExpr*>(n)->type.get());
            --p.depth;
            break;
        }
        case NodeKind::AssignmentExpr: {
            auto* a = static_cast<AssignmentExpr*>(n);
            p.line(a->isCompound
                       ? std::string("Assign(compound ") + punctToString(a->compoundOp) + ")"
                       : std::string("Assign"));
            ++p.depth;
            dumpNode(p, a->lhs.get());
            dumpNode(p, a->rhs.get());
            --p.depth;
            break;
        }
        case NodeKind::RangeExpr: {
            auto* r = static_cast<RangeExpr*>(n);
            p.line(r->halfOpen ? "Range ..<" : "Range ...");
            ++p.depth;
            dumpNode(p, r->lower.get());
            dumpNode(p, r->upper.get());
            --p.depth;
            break;
        }
        case NodeKind::GenericExpr: {
            auto* g = static_cast<GenericExpr*>(n);
            p.line("GenericSpecialization");
            ++p.depth;
            dumpNode(p, g->base.get());
            for (auto& a : g->args) dumpNode(p, a.get());
            --p.depth;
            break;
        }
        case NodeKind::MoveExpr: {
            p.line("Move");
            ++p.depth;
            dumpNode(p, static_cast<MoveExpr*>(n)->operand.get());
            --p.depth;
            break;
        }

        // ── Type representations ────────────────────────────────────────────
        case NodeKind::NamedType:
            p.line("NamedType " + static_cast<NamedType*>(n)->name); break;
        case NodeKind::OptionalType: {
            p.line("OptionalType");
            ++p.depth;
            dumpNode(p, static_cast<OptionalType*>(n)->wrapped.get());
            --p.depth;
            break;
        }
        case NodeKind::ArrayType: {
            p.line("ArrayType");
            ++p.depth;
            dumpNode(p, static_cast<ArrayType*>(n)->element.get());
            --p.depth;
            break;
        }
        case NodeKind::DictType: {
            p.line("DictType");
            ++p.depth;
            dumpNode(p, static_cast<DictType*>(n)->key.get());
            dumpNode(p, static_cast<DictType*>(n)->value.get());
            --p.depth;
            break;
        }
        case NodeKind::TupleType: {
            auto* t = static_cast<TupleType*>(n);
            p.line("TupleType (" + std::to_string(t->elements.size()) + ")");
            ++p.depth;
            for (auto& e : t->elements) dumpNode(p, e.get());
            --p.depth;
            break;
        }
        case NodeKind::FuncType: {
            p.line("FuncType");
            ++p.depth;
            for (auto& e : static_cast<FuncType*>(n)->params) dumpNode(p, e.get());
            if (static_cast<FuncType*>(n)->ret) dumpNode(p, static_cast<FuncType*>(n)->ret.get());
            --p.depth;
            break;
        }
        case NodeKind::RefType: {
            auto* r = static_cast<RefType*>(n);
            p.line("RefType " + r->refKind);
            ++p.depth;
            dumpNode(p, r->pointee.get());
            --p.depth;
            break;
        }
        case NodeKind::InoutType: {
            p.line("InoutType");
            ++p.depth;
            dumpNode(p, static_cast<InoutType*>(n)->pointee.get());
            --p.depth;
            break;
        }
        case NodeKind::NeverType: p.line("NeverType"); break;
        case NodeKind::PlaceholderType: p.line("PlaceholderType"); break;
        case NodeKind::MetatypeType: {
            p.line("MetatypeType");
            ++p.depth;
            dumpNode(p, static_cast<MetatypeType*>(n)->base.get());
            --p.depth;
            break;
        }
        // Safety net: a newly added NodeKind without a printer shows up here
        // rather than silently producing no output.
        default:
            p.line(std::string("Unhandled ") + kindName(n->kind));
            break;
    }
}

const char* kindName(NodeKind k) {
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
        case NodeKind::ActorDecl: return "ActorDecl";
        case NodeKind::BlockStmt: return "BlockStmt";
        case NodeKind::ExprStmt: return "ExprStmt";
        case NodeKind::ReturnStmt: return "ReturnStmt";
        case NodeKind::IfStmt: return "IfStmt";
        case NodeKind::IfExpr: return "IfExpr";
        case NodeKind::GuardStmt: return "GuardStmt";
        case NodeKind::WhileStmt: return "WhileStmt";
        case NodeKind::RepeatWhileStmt: return "RepeatWhileStmt";
        case NodeKind::ForInStmt: return "ForInStmt";
        case NodeKind::SwitchStmt: return "SwitchStmt";
        case NodeKind::CaseClause: return "CaseClause";
        case NodeKind::AccessorDecl: return "AccessorDecl";
        case NodeKind::TernaryExpr: return "TernaryExpr";
        case NodeKind::BreakStmt: return "BreakStmt";
        case NodeKind::FallthroughStmt: return "FallthroughStmt";
        case NodeKind::ContinueStmt: return "ContinueStmt";
        case NodeKind::DeferStmt: return "DeferStmt";
        case NodeKind::DoStmt: return "DoStmt";
        case NodeKind::CatchClause: return "CatchClause";
        case NodeKind::ThrowStmt: return "ThrowStmt";
        case NodeKind::UnsafeStmt: return "UnsafeStmt";
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
        case NodeKind::GenericExpr: return "GenericExpr";
        case NodeKind::MoveExpr: return "MoveExpr";
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

} // namespace

// ─── Commands ───────────────────────────────────────────────────────────────
static void usage(FILE* out) {
    fprintf(out,
        "sukic — SukiCode compiler driver\n"
        "\n"
        "Usage:\n"
        "  sukic lex    <file>...               dump the token stream\n"
        "  sukic parse  <file>... [--dump-ast] [--expand-macros]  parse to AST and report diagnostics\n"
        "  sukic check  <file>... [--expand-macros]  parse and run semantic analysis\n"
        "  sukic emit-ir <file>... [--target=T]  print textual LLVM IR\n"
        "  sukic build  <file> -o <out.o>       compile to a target object file\n"
        "  sukic run    <file> [--target=T]      compile, link and execute\n"
        "  sukic version                        print version information\n"
        "  sukic help                           print this help\n");
}

static int commandLex(const std::vector<std::string>& files) {
    int rc = 0;
    for (const auto& path : files) {
        std::string src;
        if (!readFile(path, src)) {
            fprintf(stderr, "sukic: cannot open '%s'\n", path.c_str());
            rc = 1;
            continue;
        }
        DiagnosticEngine diags;
        Lexer lexer(src, diags);
        auto toks = lexer.tokenizeAll();
        printf("== %s (%zu tokens) ==\n", path.c_str(), toks.size());
        for (const auto& t : toks) {
            if (t.kind == TokenKind::TK_Keyword) {
                printf("  %u:%-4u %-10s %s\n", t.loc.line, t.loc.column,
                       tokenKindName(t.kind), keywordToString(t.keyword));
            } else if (t.kind == TokenKind::TK_Punctuator) {
                printf("  %u:%-4u %-10s %s\n", t.loc.line, t.loc.column,
                       tokenKindName(t.kind), punctToString(t.punct));
            } else {
                printf("  %u:%-4u %-10s %s\n", t.loc.line, t.loc.column,
                       tokenKindName(t.kind), t.text.c_str());
            }
        }
        if (!diags.emit(stdout)) rc = 1;
    }
    return rc;
}

// 前向声明（定义见 loadImportedStdlib，位于本文件下方）。
static void loadImportedStdlib(NodeList& userDecls);

static int commandParse(const std::vector<std::string>& files, bool dumpAst, bool dumpExpanded = false) {
    int rc = 0;
    for (const auto& path : files) {
        std::string src;
        if (!readFile(path, src)) {
            fprintf(stderr, "sukic: cannot open '%s'\n", path.c_str());
            rc = 1;
            continue;
        }
        DiagnosticEngine diags;
        Lexer lexer(src, diags);
        auto toks = lexer.tokenizeAll();
        Parser parser(std::move(toks), diags);
        NodeList decls = parser.parseModule();
        if (dumpExpanded) {
            size_t uc = decls.size();
            loadImportedStdlib(decls);
            Sema sema(diags);
            sema.setStdlibDeclCount(decls.size() - uc);
            sema.setSource(&src);
            sema.expandMacros(decls);
            AstPrinter p;
            for (auto& d : decls) dumpNode(p, d.get());
        } else if (dumpAst) {
            printf("== %s ==\n", path.c_str());
            AstPrinter p;
            for (auto& d : decls) dumpNode(p, d.get());
        }
        if (!diags.emit(stderr)) rc = 1;
    }
    return rc;
}

// Parse + run semantic analysis, reporting all semantic diagnostics.
static void loadImportedStdlib(NodeList& userDecls);

static int commandCheck(const std::vector<std::string>& files, bool dumpExpanded = false) {
    int rc = 0;
    for (const auto& path : files) {
        std::string src;
        if (!readFile(path, src)) {
            fprintf(stderr, "sukic: cannot open '%s'\n", path.c_str());
            rc = 1;
            continue;
        }
        DiagnosticEngine diags;
        Lexer lexer(src, diags);
        auto toks = lexer.tokenizeAll();
        Parser parser(std::move(toks), diags);
        NodeList decls = parser.parseModule();
        // Only run Sema if parsing produced a usable tree.
        size_t userDeclCount = decls.size();
        loadImportedStdlib(decls);
        if (!diags.hasErrors()) {
            Sema sema(diags);
            sema.setStdlibDeclCount(decls.size() - userDeclCount);
            sema.setSource(&src);
            // `--expand-macros`：仅展开并 dump AST，不做完整语义分析。
            if (dumpExpanded) {
                sema.expandMacros(decls);
                AstPrinter p;
                for (auto& d : decls) dumpNode(p, d.get());
                if (!diags.emit(stderr)) rc = 1;
                continue;
            }
            sema.analyze(decls);
        }
        if (!diags.emit(stderr)) rc = 1;
    }
    return rc;
}

// Parse a source file into declarations (shared by the codegen commands).
static bool parseFile(const std::string& path, DiagnosticEngine& diags, NodeList& decls) {
    std::string src;
    if (!readFile(path, src)) {
        fprintf(stderr, "sukic: cannot open '%s'\n", path.c_str());
        return false;
    }
    Lexer lexer(src, diags);
    auto toks = lexer.tokenizeAll();
    Parser parser(std::move(toks), diags);
    decls = parser.parseModule();
    return !diags.hasErrors();
}

// Map an `import X` directive to the matching standard-library source file and
// prepend its declarations to the translation unit. This is a minimal,
// import-driven module loader; full multi-file module resolution is tracked
// separately (todo T1). Loading is opt-in: files that do not `import` a stdlib
// module compile exactly as before, so the existing test suite is unaffected.
static void loadImportedStdlib(NodeList& userDecls) {
    std::string dir = SUKI_STDLIB_DIR;
    std::string files = SUKI_STDLIB_FILES;
    if (dir.empty() || files.empty()) return;

    // Build moduleName -> absolute path from the CMake-provided file list.
    std::vector<std::pair<std::string, std::string>> mods;
    size_t start = 0;
    while (start <= files.size()) {
        size_t end = files.find('|', start);
        if (end == std::string::npos) end = files.size();
        std::string p = files.substr(start, end - start);
        if (!p.empty()) {
            std::string name = p.substr(p.find_last_of("/\\") + 1);
            const size_t ext = name.find(".suki");
            if (!name.empty() && ext != std::string::npos && ext == name.size() - 5)
                name.resize(name.size() - 5);
            mods.emplace_back(name, p);
        }
        if (end == files.size()) break;
        start = end + 1;
    }

    // Collect paths requested via `import`.
    std::vector<std::string> toLoad;
    for (auto& d : userDecls) {
        if (d && d->kind == NodeKind::ImportDecl) {
            auto* imp = static_cast<ImportDecl*>(d.get());
            for (auto& m : mods)
                if (m.first == imp->moduleName) { toLoad.push_back(m.second); break; }
        }
    }
    if (toLoad.empty()) return;

    // De-duplicate and parse + prepend.
    std::sort(toLoad.begin(), toLoad.end());
    toLoad.erase(std::unique(toLoad.begin(), toLoad.end()), toLoad.end());
    NodeList prelude;
    for (const auto& p : toLoad) {
        DiagnosticEngine d;
        NodeList dl;
        if (parseFile(p, d, dl))
            for (auto& n : dl) prelude.push_back(std::move(n));
        else
            fprintf(stderr, "sukic: warning: stdlib module '%s' failed to parse\n", p.c_str());
    }
    if (!prelude.empty())
        userDecls.insert(userDecls.begin(),
                         std::make_move_iterator(prelude.begin()),
                         std::make_move_iterator(prelude.end()));
}

static TargetInfo resolveTarget(const std::string& triple) {
    if (!triple.empty()) {
        TargetInfo t;
        if (getTargetInfo(triple, t)) return t;
        fprintf(stderr, "sukic: unknown target triple '%s'; using host\n", triple.c_str());
    }
    return hostTarget();
}

#ifndef SUKI_CLANG_DRIVER
#define SUKI_CLANG_DRIVER "clang"
#endif
#ifndef SUKI_RUNTIME_SOURCE
#define SUKI_RUNTIME_SOURCE "runtime.c"
#endif
#ifndef SUKI_RUNTIME_OBJECT
#define SUKI_RUNTIME_OBJECT "runtime.o"
#endif
#ifndef SUKI_STDLIB_DIR
#define SUKI_STDLIB_DIR ""
#endif
#ifndef SUKI_STDLIB_FILES
#define SUKI_STDLIB_FILES ""
#endif

// Compile one source file to a target object file: SukiCode -> LLVM IR, then
// clang (an LLVM frontend) lowers the IR to a native object file.
static bool compileOne(const std::string& path, const std::string& triple,
                       const std::string& objPath, std::string& errOut,
                       std::string* irOut = nullptr) {
    DiagnosticEngine diags;
    NodeList decls;
    if (!parseFile(path, diags, decls)) { diags.emit(stderr); return false; }
    size_t userDeclCount = decls.size();
    loadImportedStdlib(decls);
    Sema sema(diags);
    sema.setStdlibDeclCount(decls.size() - userDeclCount);
    // 宏展开需要调用点源码以恢复 unquote 的原始文本（规范 5.6）。
    std::string src;
    readFile(path, src);
    sema.setSource(&src);
    sema.analyze(decls);
    if (diags.hasErrors()) { diags.emit(stderr); return false; }
    IRGenerator gen(resolveTarget(triple));
    std::string ir, err;
    // BLK-B: lower SukiCode -> LLVM IR -> native object entirely in-process via
    // LLVM's TargetMachine/MC backend. No external `clang -c` is invoked, so the
    // driver no longer depends on a C/IR frontend to produce an object file. The
    // textual IR is still produced when requested (used to locate `@main`).
    if (!gen.emitObject(decls, sema, objPath, irOut ? &ir : nullptr, err)) {
        errOut = err;
        return false;
    }
    if (irOut) *irOut = ir;
    return true;
}

static int commandEmitIR(const std::vector<std::string>& files, const std::string& triple) {
    int rc = 0;
    for (const auto& path : files) {
        DiagnosticEngine diags;
        NodeList decls;
        if (!parseFile(path, diags, decls)) { diags.emit(stderr); rc = 1; continue; }
        size_t userDeclCount = decls.size();
        loadImportedStdlib(decls);
        Sema sema(diags);
        sema.setStdlibDeclCount(decls.size() - userDeclCount);
        sema.analyze(decls);
        if (diags.hasErrors()) { diags.emit(stderr); rc = 1; continue; }
        IRGenerator gen(resolveTarget(triple));
        std::string ir, err;
        if (!gen.emitIR(decls, sema, ir, err)) { fprintf(stderr, "sukic: %s\n", err.c_str()); rc = 1; continue; }
        printf("; %s\n%s", path.c_str(), ir.c_str());
    }
    return rc;
}

static int commandBuild(const std::vector<std::string>& files, const std::string& out,
                        const std::string& triple) {
    if (files.size() != 1) { fprintf(stderr, "sukic: build currently takes one input file\n"); return 2; }
    std::string err;
    if (!compileOne(files[0], triple, out, err)) { fprintf(stderr, "sukic: %s\n", err.c_str()); return 1; }
    return 0;
}

static int commandRun(const std::vector<std::string>& files, const std::string& triple) {
    if (files.size() != 1) { fprintf(stderr, "sukic: run currently takes one input file\n"); return 2; }
    std::string obj = files[0] + ".o";
    std::string exe = files[0] + ".out";
    std::string err, ir;
    if (!compileOne(files[0], triple, obj, err, &ir)) { fprintf(stderr, "sukic: %s\n", err.c_str()); return 1; }
    // A translation unit without `@main` compiles fine but cannot be linked
    // into a program. Say so instead of letting the linker report a missing
    // `main`, which looks like a compiler bug.
    if (ir.find("define i32 @main") == std::string::npos) {
        fprintf(stderr,
                "sukic: no @main entry in '%s' — it compiles as a module but "
                "cannot be linked into an executable\n",
                files[0].c_str());
        return 1;
    }
    // BLK-B: the object was produced in-process by LLVM; link it with the
    // precompiled C runtime object. When lld is installed we ask the clang driver
    // to use it as the linker (`-fuse-ld=lld`), so the actual link is performed
    // by lld while clang still supplies the correct crt/libc search paths.
    // Otherwise the default system linker (via the clang driver) is used.
    std::string linker = std::string(SUKI_CLANG_DRIVER);
#ifdef SUKI_PREFER_LLD
    linker += " -fuse-ld=lld";
#endif
    // Threading libraries are named per platform: Linux needs an explicit
    // -lpthread, macOS resolves pthreads from libSystem and Windows needs
    // nothing, so the link line stays correct on each without extra flags.
    std::string linkLibs;
#if defined(__linux__)
    linkLibs = " -lpthread";
#elif defined(__APPLE__)
    linkLibs = "";
#elif defined(_WIN32)
    linkLibs = "";
#endif
    std::string link = linker + linkLibs + " '" + obj + "' '" +
                       SUKI_RUNTIME_OBJECT + "' -o '" + exe + "'";
    if (std::system(link.c_str()) != 0) { fprintf(stderr, "sukic: link failed\n"); return 1; }
    // `sh -c "name"` searches PATH, not the current directory, so a relative
    // executable must be invoked as ./name.
    std::string exePath = (exe.empty() || exe[0] == '/') ? exe : ("./" + exe);
    int rc = std::system(exePath.c_str());
    // std::system returns a wait status (exit code in the high byte); convert
    // it to the child's actual exit code.
    return (rc >> 8) & 0xFF;
}

int main(int argc, char** argv) {
    if (argc < 2) { usage(stderr); return 2; }

    std::string cmd = argv[1];
    if (cmd == "--version" || cmd == "-v" || cmd == "version") {
        printf("sukic %s\n", kVersion);
        return 0;
    }
    if (cmd == "--help" || cmd == "-h" || cmd == "help") {
        usage(stdout);
        return 0;
    }

    std::vector<std::string> files;
    bool dumpAst = false;
    bool dumpExpanded = false;
    std::string triple, outPath;
    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--dump-ast") == 0) dumpAst = true;
        else if (strcmp(argv[i], "--expand-macros") == 0) dumpExpanded = true;
        else if (strncmp(argv[i], "--target=", 9) == 0) triple = argv[i] + 9;
        else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) outPath = argv[++i];
        else files.emplace_back(argv[i]);
    }

    if (cmd == "lex") {
        if (files.empty()) { fprintf(stderr, "sukic: lex requires at least one file\n"); return 2; }
        return commandLex(files);
    }
    if (cmd == "parse") {
        if (files.empty()) { fprintf(stderr, "sukic: parse requires at least one file\n"); return 2; }
        return commandParse(files, dumpAst, dumpExpanded);
    }
    if (cmd == "check") {
        if (files.empty()) { fprintf(stderr, "sukic: check requires at least one file\n"); return 2; }
        return commandCheck(files, dumpExpanded);
    }
    if (cmd == "emit-ir") {
        if (files.empty()) { fprintf(stderr, "sukic: emit-ir requires at least one file\n"); return 2; }
        return commandEmitIR(files, triple);
    }
    if (cmd == "build") {
        if (files.empty()) { fprintf(stderr, "sukic: build requires one input file\n"); return 2; }
        if (outPath.empty()) outPath = "a.out.o";
        return commandBuild(files, outPath, triple);
    }
    if (cmd == "run") {
        if (files.empty()) { fprintf(stderr, "sukic: run requires one input file\n"); return 2; }
        return commandRun(files, triple);
    }

    fprintf(stderr, "sukic: unknown command '%s'\n\n", cmd.c_str());
    usage(stderr);
    return 2;
}
