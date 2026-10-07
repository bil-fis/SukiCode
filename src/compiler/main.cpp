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
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <filesystem>
#include <map>
#include <set>
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
        case NodeKind::LoopStmt: {
            auto* l = static_cast<LoopStmt*>(n);
            p.line(std::string("Loop") + (l->label.empty() ? "" : " '" + l->label + "'"));
            ++p.depth;
            for (auto& st : l->body) dumpNode(p, st.get());
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
        case NodeKind::LoopStmt: return "LoopStmt";
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
        "  sukic lex    <file>...                 dump the token stream\n"
        "  sukic parse  <file>... [--emit-ast] [--expand-macros]   parse to AST and report diagnostics\n"
        "  sukic check  <file>... [--expand-macros]  parse and run semantic analysis\n"
        "  sukic emit-ir <file>... [--target=T]   print textual LLVM IR\n"
        "  sukic build  <file> [-o <out>]         compile to a target object/asm file\n"
        "  sukic run    <file> [--target=T]       compile, link and execute\n"
        "  sukic -o <out> <file.suki>             compile + link (no run)\n"
        "  sukic -c -o <out.o> <file.suki>        compile only (no link)\n"
        "  sukic --list-targets                   list supported target triples\n"
        "\n"
        "Common options (规范 §10.3 / §10.4 / §14.1):\n"
        "  --target=<triple>          目标三元组 (e.g. x86_64-pc-windows-msvc, aarch64-apple-darwin)\n"
        "  --sysroot=<path>           交叉链接系统库根目录\n"
        "  -D NAME[=VALUE]            条件编译自定义宏 (e.g. -D LEVEL=5)\n"
        "  -O0 / -O1 / -O2 / -O3      优化级别\n"
        "  -Os                        尺寸优先优化\n"
        "  -c                         仅编译到对象文件 (不链接)\n"
        "  -S                         输出汇编文本 (.s)\n"
        "  -emit-ir / --emit-llvm     输出文本 LLVM IR\n"
        "  -emit-ast                  输出语法树\n"
        "  -expand-macros             展开宏后输出\n"
        "  --incremental              .build/cache 增量编译\n"
        "  -fuse-ld=lld              使用 lld 链接 (默认)\n"
        "  --integrated-as / --no-integrated-as  (LLVM 集成汇编器，默认开启)\n"
        "  --target bare-metal        禁用标准库，仅用 core (裸机，实验性)\n"
        "  -o <path>                  输出路径\n"
        "  --version / -v, --help / -h\n");
}

// 规范驱动层级的编译选项集合：聚合所有 CLI 标志，传给编译管线各阶段。
struct CompileOptions {
    std::string triple;             // --target=<triple>（空 = 主机）
    std::string outPath;           // -o <path>
    std::vector<std::string> defines; // -D NAME[=VALUE]（规范 §10.3）
    int  optLevel   = 0;           // -O0..-O3 → LLVM CodeGenOptLevel
    bool optSize    = false;        // -Os（尺寸优先）
    bool emitAsm    = false;        // -S（汇编文本而非对象文件）
    bool compileOnly= false;        // -c（仅编译到 .o/.s，不链接）
    bool emitIR     = false;        // -emit-ir / --emit-llvm（文本 LLVM IR）
    bool emitAST    = false;        // -emit-ast（语法树转储）
    bool expandMacros = false;      // -expand-macros（宏展开后转储）
    bool incremental = false;       // --incremental（.build/cache 缓存）
    std::string sysroot;            // --sysroot=<path>（交叉链接系统库根）
    bool fuseLld    = false;        // -fuse-ld=lld（默认行为，可显式指定）
    bool integratedAs = true;       // --integrated-as（默认）/--no-integrated-as
    bool bareMetal  = false;        // --target bare-metal（禁用标准库，仅 core）
};

// 解析用于代码生成的有效三元组。规范 §8.7：`--target bare-metal` 是禁用
// 标准库、仅使用 core 模块的约定目标名，映射到 freestanding 三元组。
static std::string effectiveTriple(const CompileOptions& opts) {
    if (opts.triple == "bare-metal" || opts.triple == "none") {
        TargetInfo h = hostTarget();
        return h.arch + "-unknown-none";
    }
    return opts.triple;
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

static int commandParse(const std::vector<std::string>& files, const CompileOptions& opts) {
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
        lexer.setTargetTriple(effectiveTriple(opts));
        if (!opts.defines.empty()) lexer.setDefines(opts.defines);
        auto toks = lexer.tokenizeAll();
        Parser parser(std::move(toks), diags);
        NodeList decls = parser.parseModule();
        if (opts.expandMacros) {
            size_t uc = decls.size();
            loadImportedStdlib(decls);
            Sema sema(diags);
            sema.setStdlibDeclCount(decls.size() - uc);
            sema.setSource(&src);
            sema.expandMacros(decls);
            AstPrinter p;
            for (auto& d : decls) dumpNode(p, d.get());
        } else if (opts.emitAST) {
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

static int commandCheck(const std::vector<std::string>& files, const CompileOptions& opts) {
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
        lexer.setTargetTriple(effectiveTriple(opts));
        if (!opts.defines.empty()) lexer.setDefines(opts.defines);
        auto toks = lexer.tokenizeAll();
        Parser parser(std::move(toks), diags);
        NodeList decls = parser.parseModule();
        // Only run Sema if parsing produced a usable tree.
        size_t userDeclCount = decls.size();
        if (!opts.bareMetal) loadImportedStdlib(decls);
        if (!diags.hasErrors()) {
            Sema sema(diags);
            sema.setStdlibDeclCount(decls.size() - userDeclCount);
            sema.setSource(&src);
            // `--expand-macros`：仅展开并 dump AST，不做完整语义分析。
            if (opts.expandMacros) {
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
// `triple` 用于条件编译的 os()/arch() 谓词（规范 §10.3）；为空时 Lexer 回退到
// 主机目标，因此 lex/parse/check 等不涉及交叉编译的命令可保持默认。
// `defines` 为 `-D NAME[=VALUE]` 注入的自定义宏（规范 §10.3）。
static bool parseFile(const std::string& path, DiagnosticEngine& diags, NodeList& decls,
                      const std::string& triple = std::string(),
                      const std::vector<std::string>& defines = {}) {
    std::string src;
    if (!readFile(path, src)) {
        fprintf(stderr, "sukic: cannot open '%s'\n", path.c_str());
        return false;
    }
    Lexer lexer(src, diags);
    lexer.setTargetTriple(triple);
    if (!defines.empty()) lexer.setDefines(defines);
    auto toks = lexer.tokenizeAll();
    Parser parser(std::move(toks), diags);
    decls = parser.parseModule();
    return !diags.hasErrors();
}

// Map an `import X` directive to the matching standard-library source file and
// prepend its declarations to the translation unit. This is an import-driven
// module loader that resolves a module's own `import` statements recursively
// (transitive dependencies), with cycle detection so two modules that import
// each other (e.g. `core` <-> `system`) do not loop forever. Loading stays
// opt-in: files that do not `import` any stdlib module compile exactly as
// before, so the existing test suite is unaffected.
// ── 重定位支持 ───────────────────────────────────────────────────────────────
// 允许通过环境变量覆盖编译期写死的绝对路径，使编译器可被打包为独立目录
// （开箱即用）。未设置环境变量时回退到 CMake 写入的默认路径，原有行为不变。
static std::string relocEnv(const char* name, const char* fallback) {
    const char* v = std::getenv(name);
    return (v && *v) ? std::string(v) : std::string(fallback);
}

// 递归收集 dir 下所有 *.suki 标准库源文件，用 '|' 连接（与 SUKI_STDLIB_FILES 一致）。
static std::string globStdlibFiles(const std::string& dir) {
    std::string out;
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) return out;
    for (auto it = std::filesystem::recursive_directory_iterator(dir, ec);
         it != std::filesystem::recursive_directory_iterator(); ++it) {
        const auto& p = it->path();
        if (p.extension() == ".suki") {
            if (!out.empty()) out += '|';
            out += p.string();
        }
    }
    return out;
}

static void loadImportedStdlib(NodeList& userDecls) {
    // 若设置了 SUKICODE_STDLIB_DIR，则从该目录重新发现标准库（支持重定位打包）；
    // 否则使用 CMake 编译期写入的默认文件清单。
    const char* envStdlib = std::getenv("SUKICODE_STDLIB_DIR");
    std::string dir = relocEnv("SUKICODE_STDLIB_DIR", SUKI_STDLIB_DIR);
    std::string files = envStdlib && *envStdlib
                            ? globStdlibFiles(dir)
                            : std::string(SUKI_STDLIB_FILES);
    if (dir.empty() || files.empty()) return;

    // Build moduleName -> absolute path from the CMake-provided file list.
    std::map<std::string, std::string> modPath;
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
            modPath[name] = p;
        }
        if (end == files.size()) break;
        start = end + 1;
    }

    // Transitive import resolution with cycle detection.
    // `worklist` holds module names still to be parsed; `seen` records every
    // module already enqueued so a module is parsed at most once and an
    // `import` cycle cannot cause unbounded recursion.
    std::vector<std::string> worklist;
    std::set<std::string> seen;
    for (auto& d : userDecls) {
        if (d && d->kind == NodeKind::ImportDecl) {
            std::string nm = static_cast<ImportDecl*>(d.get())->moduleName;
            if (modPath.count(nm) && seen.insert(nm).second)
                worklist.push_back(nm);
        }
    }

    // name -> parsed declarations (parsed exactly once per module).
    std::map<std::string, NodeList> parsed;
    while (!worklist.empty()) {
        std::string nm = worklist.back();
        worklist.pop_back();
        std::string path = modPath[nm];
        DiagnosticEngine d;
        NodeList dl;
        if (parseFile(path, d, dl)) {
            parsed[nm] = std::move(dl);
            // Enqueue this module's own imports (recursive resolution). The
            // Sema phase ignores ImportDecl nodes, and `seen` above prevents
            // both double-loading and cycles.
            for (auto& n : parsed[nm]) {
                if (n && n->kind == NodeKind::ImportDecl) {
                    std::string child = static_cast<ImportDecl*>(n.get())->moduleName;
                    if (modPath.count(child) && seen.insert(child).second)
                        worklist.push_back(child);
                }
            }
        } else {
            fprintf(stderr, "sukic: warning: stdlib module '%s' failed to parse\n",
                    path.c_str());
        }
    }

    // Prepend every loaded module's declarations. `seen` ensures a module's
    // transitive imports are loaded before the module itself is consumed, so
    // dependencies always precede their dependents. 规范 §10.1：给被导入模块
    // 的声明打上模块名，访问据此区分「同模块」与「跨模块」（跨模块仅 public/
    // open 可见）。ImportDecl/ModuleDecl 节点由 Sema 的 default 分支忽略，
    // 故一并保留无副作用。
    NodeList prelude;
    for (auto& kv : parsed) {
        for (auto& n : kv.second) {
            if (n) n->sourceModule = kv.first;
            prelude.push_back(std::move(n));
        }
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
#ifndef SUKI_RUNTIME_OBJECT
#define SUKI_RUNTIME_OBJECT "runtime.o"
#endif
#ifndef SUKI_RUNTIME_SOURCE
#define SUKI_RUNTIME_SOURCE "runtime.c"
#endif
#ifndef SUKI_RUNTIME_INCLUDE
#define SUKI_RUNTIME_INCLUDE "."
#endif
#ifndef SUKI_STDLIB_DIR
#define SUKI_STDLIB_DIR ""
#endif
#ifndef SUKI_STDLIB_FILES
#define SUKI_STDLIB_FILES ""
#endif

// 规范 §14.1.2：--incremental，把对象/汇编按「源内容 + 编译选项」哈希缓存到
// .build/cache，命中时直接复用，避免重复代码生成。返回缓存文件路径。
static std::string incrementalCachePath(const std::string& path, const CompileOptions& opts,
                                        const std::string& src) {
    std::string key = src;
    key += "\n@@triple@@" + effectiveTriple(opts);
    key += "\n@@opt@@" + std::to_string(opts.optLevel) + "," +
           std::to_string(opts.optSize) + "," + std::to_string(opts.emitAsm) +
           "," + std::to_string(opts.bareMetal);
    for (const auto& d : opts.defines) key += "\n@@def@@" + d;
    size_t h = std::hash<std::string>{}(key);
    return ".build/cache/" + std::to_string(h) + (opts.emitAsm ? ".s" : ".o");
}

// Compile one source file to a target object/assembly file: SukiCode -> LLVM IR
// -> native object (或 -S 时的汇编) via the in-process LLVM backend.
static bool compileOne(const std::string& path, const std::string& objPath,
                       std::string& errOut, std::string* irOut,
                       const CompileOptions& opts) {
    // 宏展开需要调用点源码以恢复 unquote 的原始文本（规范 5.6）；同时用于增量缓存键。
    std::string src;
    readFile(path, src);

    // 增量编译命中：直接复制缓存对象到目标路径。
    if (opts.incremental) {
        std::string cachePath = incrementalCachePath(path, opts, src);
        std::ifstream cf(cachePath, std::ios::binary);
        if (cf) {
            std::error_code ec;
            std::filesystem::copy_file(cachePath, objPath,
                std::filesystem::copy_options::overwrite_existing, ec);
            if (ec) { fprintf(stderr, "sukic: incremental cache copy failed: %s\n", ec.message().c_str()); }
            if (irOut) *irOut = "";
            return !ec;
        }
    }

    DiagnosticEngine diags;
    NodeList decls;
    if (!parseFile(path, diags, decls, effectiveTriple(opts), opts.defines)) {
        diags.emit(stderr); return false;
    }
    size_t userDeclCount = decls.size();
    // 裸机模式（规范 §8.7）不加载标准库，仅保留用户声明。
    if (!opts.bareMetal) loadImportedStdlib(decls);
    Sema sema(diags);
    sema.setStdlibDeclCount(decls.size() - userDeclCount);
    sema.setSource(&src);
    sema.analyze(decls);
    if (diags.hasErrors()) { diags.emit(stderr); return false; }
    IRGenerator gen(resolveTarget(effectiveTriple(opts)));
    gen.setOptLevel(opts.optLevel);
    gen.setOptSize(opts.optSize);
    gen.setEmitAssembly(opts.emitAsm);
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
    // 写入增量缓存（下次同输入命中）。
    if (opts.incremental) {
        std::error_code ec;
        std::filesystem::create_directories(".build/cache", ec);
        std::filesystem::copy_file(objPath, incrementalCachePath(path, opts, src),
            std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) fprintf(stderr, "sukic: warning: cannot write incremental cache: %s\n", ec.message().c_str());
    }
    return true;
}

static int commandEmitIR(const std::vector<std::string>& files, const CompileOptions& opts) {
    int rc = 0;
    for (const auto& path : files) {
        DiagnosticEngine diags;
        NodeList decls;
        if (!parseFile(path, diags, decls, effectiveTriple(opts), opts.defines)) { diags.emit(stderr); rc = 1; continue; }
        size_t userDeclCount = decls.size();
        if (!opts.bareMetal) loadImportedStdlib(decls);
        Sema sema(diags);
        sema.setStdlibDeclCount(decls.size() - userDeclCount);
        sema.analyze(decls);
        if (diags.hasErrors()) { diags.emit(stderr); rc = 1; continue; }
        IRGenerator gen(resolveTarget(effectiveTriple(opts)));
        std::string ir, err;
        if (!gen.emitIR(decls, sema, ir, err)) { fprintf(stderr, "sukic: %s\n", err.c_str()); rc = 1; continue; }
        // 规范 §14.1：`-emit-ir` / `--emit-llvm` 输出文本 LLVM IR；给定 -o 写入文件，否则输出到 stdout。
        if (!opts.outPath.empty()) {
            std::ofstream o(opts.outPath, std::ios::binary);
            if (!o) { fprintf(stderr, "sukic: cannot write IR to '%s'\n", opts.outPath.c_str()); rc = 1; continue; }
            o << ir;
        } else {
            printf("; %s\n%s", path.c_str(), ir.c_str());
        }
    }
    return rc;
}

// 仅编译阶段（规范 §14.1.3 的 `-c` / 默认 build）：产出对象文件或汇编文本，不链接。
static int commandBuild(const std::vector<std::string>& files, const CompileOptions& opts) {
    if (files.size() != 1) { fprintf(stderr, "sukic: build currently takes one input file\n"); return 2; }
    std::string out = opts.outPath;
    if (out.empty()) out = files[0] + (opts.emitAsm ? ".s" : ".o");
    std::string err;
    if (!compileOne(files[0], out, err, nullptr, opts)) { fprintf(stderr, "sukic: %s\n", err.c_str()); return 1; }
    return 0;
}

// 跨平台 shell 参数引用：POSIX 用单引号，Windows(cmd.exe)用双引号并转义内嵌引号。
static std::string shellQuote(const std::string& s) {
#ifdef _WIN32
    std::string out = "\"";
    for (char c : s) { if (c == '"') out += "\"\"";
        else out += c; }
    out += "\"";
    return out;
#else
    std::string out = "'";
    for (char c : s) { if (c == '\'') out += "'\\''";
        else out += c; }
    out += "'";
    return out;
#endif
}

// 为目标平台选择运行时对象：native（OS+arch 与宿主一致）直接用预编译产物；
// 交叉编译目标则尝试按 --target 即时编译 runtime.c（需要目标 sysroot/工具链），
// 失败时返回空串并以 err 说明原因（调用方据此报错，但仍允许 `build` 先产出对象）。
// 裸机模式（规范 §8.7）不使用 C 运行时对象。
static std::string selectRuntimeObject(const TargetInfo& tgt, const TargetInfo& host,
                                       const std::string& objPath, std::string& err,
                                       const CompileOptions& opts) {
    if (opts.bareMetal) return "";  // 裸机自管启动/panic，无需链接 C 运行时
    if (tgt.os == host.os && tgt.arch == host.arch)
        return relocEnv("SUKICODE_RUNTIME_OBJECT", SUKI_RUNTIME_OBJECT);
    std::string rtSrc = relocEnv("SUKICODE_RUNTIME_SOURCE", SUKI_RUNTIME_SOURCE);
    std::string rtInc = relocEnv("SUKICODE_RUNTIME_INCLUDE", SUKI_RUNTIME_INCLUDE);
    std::string tmp = objPath + ".rt.o";
    std::string cc = std::string(relocEnv("SUKICODE_CLANG_DRIVER", SUKI_CLANG_DRIVER)) +
                     " --target=" + tgt.triple +
                     (opts.sysroot.empty() ? "" : (" --sysroot=" + shellQuote(opts.sysroot))) +
                     " -c -I " + shellQuote(rtInc) + " " +
                     shellQuote(rtSrc) + " -o " + shellQuote(tmp);
    if (std::system(cc.c_str()) != 0) {
        err = "无法为交叉目标 '" + tgt.triple +
              "' 编译 C 运行时（交叉链接需要对应的目标工具链/sysroot，例如带匹配头文件的 "
              "clang --target）。对象文件已产出，但可执行文件未能链接。";
        return "";
    }
    return tmp;
}

// 编译 + 链接成可执行文件；execute=true 时链接后直接运行（规范 §14.1 的 `run`）。
static int commandLinkAndRun(const std::vector<std::string>& files, const CompileOptions& opts,
                             bool execute) {
    if (files.size() != 1) { fprintf(stderr, "sukic: run currently takes one input file\n"); return 2; }
    std::string obj = files[0] + ".o";
    std::string exe = opts.outPath.empty() ? (files[0] + ".out") : opts.outPath;
    std::string err, ir;
    if (!compileOne(files[0], obj, err, &ir, opts)) { fprintf(stderr, "sukic: %s\n", err.c_str()); return 1; }
    // A translation unit without `@main` compiles fine but cannot be linked
    // into a program. Say so instead of letting the linker report a missing
    // `main`, which looks like a compiler bug. (增量缓存命中时 ir 为空，跳过此检查。)
    if (!ir.empty() && ir.find("define i32 @main") == std::string::npos) {
        fprintf(stderr,
                "sukic: no @main entry in '%s' — it compiles as a module but "
                "cannot be linked into an executable\n",
                files[0].c_str());
        return 1;
    }
    // 链接需按*目标*平台（而非宿主）选择链接器标志与运行时对象。原生构建时
    // 目标 == 宿主，行为与以往一致；交叉编译（--target 指向其它 OS/arch）时按
    // 目标三元组即时编译运行时并尝试链接，缺失 sysroot 时给出清晰错误。
    TargetInfo tgt = resolveTarget(effectiveTriple(opts));
    TargetInfo host = hostTarget();
    std::string linker = relocEnv("SUKICODE_CLANG_DRIVER", SUKI_CLANG_DRIVER);
    if (opts.fuseLld) linker += " -fuse-ld=lld";
    // 始终向 clang 传递目标三元组，使其选择对应对象格式 / crt / 链接器风格
    // （COFF+MachO 由 lld 处理，ELF 由系统链接器处理）。
    std::string targetFlag = opts.triple.empty() ? "" : (" --target=" + opts.triple);
    std::string sysrootFlag = opts.sysroot.empty() ? "" : (" --sysroot=" + shellQuote(opts.sysroot));
    // 依据*目标* OS 选择线程库：Linux/Android/FreeBSD 需 -lpthread；
    // Windows / macOS / iOS / WASI 由系统库自带，无需额外标志；裸机不链接任何库。
    std::string linkLibs;
    if (!opts.bareMetal &&
        (tgt.os == "Linux" || tgt.os == "Android" || tgt.os == "FreeBSD"))
        linkLibs = " -lpthread";
    std::string rtErr;
    std::string runtimeObj = selectRuntimeObject(tgt, host, obj, rtErr, opts);
    if (runtimeObj.empty() && !opts.bareMetal) { fprintf(stderr, "sukic: %s\n", rtErr.c_str()); return 1; }
    std::string link = linker + targetFlag + sysrootFlag + linkLibs + " " +
                       shellQuote(obj) + (runtimeObj.empty() ? "" : (" " + shellQuote(runtimeObj))) +
                       " -o " + shellQuote(exe);
    if (std::system(link.c_str()) != 0) {
        fprintf(stderr, "sukic: 链接失败（目标 '%s'）\n", opts.triple.c_str());
        return 1;
    }
    if (!execute) return 0;
    // `sh -c "name"` searches PATH, not the current directory, so a relative
    // executable must be invoked as ./name.
    std::string exePath = (exe.empty() || exe[0] == '/') ? exe : ("./" + exe);
    int rc = std::system(exePath.c_str());
    // std::system returns a wait status (exit code in the high byte); convert
    // it to the child's actual exit code.
    return (rc >> 8) & 0xFF;
}

// 规范 §14.1.3 的 `-O` 优化级别解析：`-O0`/`-O`/`-O1`/`-O2`/`-O3` 及 `-Os`（尺寸）。
static int parseOptLevel(const char* arg, bool& optSize) {
    if (strcmp(arg, "-O0") == 0) return 0;
    if (strcmp(arg, "-O") == 0 || strcmp(arg, "-O1") == 0) return 1;
    if (strcmp(arg, "-O2") == 0) return 2;
    if (strcmp(arg, "-O3") == 0) return 3;
    if (strcmp(arg, "-Os") == 0) { optSize = true; return 2; }
    return -1; // 非 -O* 标志
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
    // 规范 §14.1.1：`--list-targets` 列出已知目标三元组。
    if (cmd == "--list-targets") {
        for (const auto& t : listKnownTargets())
            printf("%s\n", t.c_str());
        return 0;
    }

    // 解析所有通用编译选项（规范 §10.3 / §10.4 / §14.1）。
    CompileOptions opts;
#ifdef SUKI_PREFER_LLD
    opts.fuseLld = true;
#endif
    std::vector<std::string> files;
    bool isSubcommand = (cmd == "lex" || cmd == "parse" || cmd == "check" ||
                         cmd == "emit-ir" || cmd == "build" || cmd == "run");
    for (int i = (isSubcommand ? 2 : 1); i < argc; ++i) {
        const char* a = argv[i];
        if (strcmp(a, "--target") == 0 && i + 1 < argc) { opts.triple = argv[++i]; }
        else if (strncmp(a, "--target=", 9) == 0) opts.triple = a + 9;
        else if (strncmp(a, "--sysroot=", 10) == 0) opts.sysroot = a + 10;
        else if (strcmp(a, "--sysroot") == 0 && i + 1 < argc) opts.sysroot = argv[++i];
        else if (strcmp(a, "-o") == 0 && i + 1 < argc) opts.outPath = argv[++i];
        else if (strcmp(a, "-c") == 0) opts.compileOnly = true;
        else if (strcmp(a, "-S") == 0) opts.emitAsm = true;
        else if (strcmp(a, "-emit-ir") == 0 || strcmp(a, "--emit-llvm") == 0) opts.emitIR = true;
        else if (strcmp(a, "-emit-ast") == 0 || strcmp(a, "--dump-ast") == 0) opts.emitAST = true;
        else if (strcmp(a, "-expand-macros") == 0) opts.expandMacros = true;
        else if (strcmp(a, "--incremental") == 0) opts.incremental = true;
        else if (strcmp(a, "--integrated-as") == 0) opts.integratedAs = true;
        else if (strcmp(a, "--no-integrated-as") == 0) {
            // LLVM 后端始终使用集成汇编器（规范 §14.1.3），外部汇编器不可用。
            opts.integratedAs = false;
            fprintf(stderr, "sukic: warning: --no-integrated-as is not supported; using LLVM integrated assembler\n");
        }
        else if (strncmp(a, "-D", 2) == 0) {
            // 规范 §10.3：`-D FLAG` 或 `-DFLAG=VALUE`（空格或等号分隔均可）。
            std::string def = a + 2;
            if (def.empty() && i + 1 < argc) def = argv[++i];
            opts.defines.push_back(def);
        }
        else if (strncmp(a, "-fuse-ld=", 9) == 0) {
            opts.fuseLld = (strcmp(a + 9, "lld") == 0);
        }
        else if (strncmp(a, "-O", 2) == 0) {
            int lv = parseOptLevel(a, opts.optSize);
            if (lv >= 0) opts.optLevel = lv;
            else {
                // 未知的 -O* 形式：回退到 O2 并打印提示。
                opts.optLevel = 2;
                fprintf(stderr, "sukic: warning: unknown optimization flag '%s', using -O2\n", a);
            }
        }
        else if (a[0] == '-' && a[1] != '\0' && !isSubcommand) {
            fprintf(stderr, "sukic: unknown flag '%s'\n", a);
            return 2;
        }
        else if (cmd == "lex" && a[0] == '-') {
            fprintf(stderr, "sukic: unknown flag '%s'\n", a);
            return 2;
        }
        else files.emplace_back(a);
    }
    // 规范 §8.7：`--target bare-metal` 映射到 freestanding 并禁用标准库。
    if (opts.triple == "bare-metal") opts.bareMetal = true;

    // ── 子命令分发 ───────────────────────────────────────────────────────────
    if (cmd == "lex") {
        if (files.empty()) { fprintf(stderr, "sukic: lex requires at least one file\n"); return 2; }
        return commandLex(files);
    }
    if (cmd == "parse") {
        if (files.empty()) { fprintf(stderr, "sukic: parse requires at least one file\n"); return 2; }
        return commandParse(files, opts);
    }
    if (cmd == "check") {
        if (files.empty()) { fprintf(stderr, "sukic: check requires at least one file\n"); return 2; }
        return commandCheck(files, opts);
    }
    if (cmd == "emit-ir") {
        if (files.empty()) { fprintf(stderr, "sukic: emit-ir requires at least one file\n"); return 2; }
        return commandEmitIR(files, opts);
    }
    if (cmd == "build") {
        if (files.empty()) { fprintf(stderr, "sukic: build requires one input file\n"); return 2; }
        return commandBuild(files, opts);
    }
    if (cmd == "run") {
        if (files.empty()) { fprintf(stderr, "sukic: run requires one input file\n"); return 2; }
        return commandLinkAndRun(files, opts, /*execute=*/true);
    }

    // ── 直接调用模式（规范 §14.1）：`sukic -o out main.suki` / `sukic -c -o out.o main.suki` ──
    if (files.empty()) { usage(stderr); return 2; }
    if (opts.emitAST || opts.expandMacros) return commandParse(files, opts);
    if (opts.emitIR) return commandEmitIR(files, opts);
    if (opts.compileOnly || opts.emitAsm) return commandBuild(files, opts);
    // 默认：编译并链接到 -o（或 a.out），不自动运行（运行请用 `sukic run`）。
    return commandLinkAndRun(files, opts, /*execute=*/false);
}
