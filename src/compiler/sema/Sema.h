#pragma once

// Semantic analysis for SukiCode.
//
// Sema consumes the AST produced by the parser and builds:
//   * a scope-resolved symbol table (locals, params, globals, types, cases),
//   * semantic types for every expression (via sema/Type.h),
//   * control-flow facts (does a block/function definitely return?),
//   * ownership facts (a value moved with `move` is no longer usable),
//   * protocol conformance facts (delegated to sema/TraitResolver).
//
// It reports user-facing errors through the shared DiagnosticEngine and never
// throws.

#include "compiler/ast/AST.h"
#include "compiler/diag/DiagnosticEngine.h"
#include "compiler/sema/Type.h"
#include "compiler/util/ScopedTable.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace suki {

// ─── Named type descriptions ──────────────────────────────────────────────
enum class TypeDeclKind { Struct, Enum, Class, Actor, Protocol, Typealias };

// A record describing one named type declaration (struct/enum/class/actor/
// protocol). Referenced by semantic `Named` types so downstream stages can
// inspect layout, inheritance and conformance.
struct TypeRecord {
    std::string name;
    TypeDeclKind kind = TypeDeclKind::Struct;
    Node* decl = nullptr;                        // AST TypeDecl node
    std::vector<std::string> genericParams;      // T, U, ...
    std::vector<GenericConstraint> genericConstraints; // 泛型约束（规范 2.1）
    const TypeRecord* superclass = nullptr;      // class single inheritance
    std::vector<const TypeRecord*> protocols;    // declared conformances
    // Members are keyed by name; a property and a method may share a name only
    // via distinct overloads, which we keep in one list and match by arity.
    struct Member {
        bool isFunction = false;
        std::string name;
        const Type* type = nullptr;  // property type, or function type
        Node* decl = nullptr;
        bool isLet = false;
        bool isStatic = false;
        // 访问控制级别与成员所属类型（规范 10.1）。`owner` 指向包含本成员的
        // TypeRecord；`private` 成员仅当 currentType_ 等于 owner 时可见。
        AccessLevel access = AccessLevel::Internal;
        const TypeRecord* owner = nullptr;
    };
    std::vector<Member> members;
    // Enum cases (name → associated value types).
    struct EnumCaseInfo {
        std::string name;
        std::vector<const Type*> associated;
    };
    std::vector<EnumCaseInfo> cases;
    // Protocol requirements (name + isFunction); satisfied by members.
    std::vector<Member> requirements;
    // Associated types declared by a protocol.
    std::vector<std::string> associatedTypes;

    bool isValueType() const {
        return kind == TypeDeclKind::Struct || kind == TypeDeclKind::Enum ||
               kind == TypeDeclKind::Typealias;
    }
    bool isProtocol() const { return kind == TypeDeclKind::Protocol; }
};

// ─── Symbols ───────────────────────────────────────────────────────────────
struct Symbol {
    enum class Kind { Variable, Parameter, Function, Type, EnumCase, Macro };
    Kind kind = Kind::Variable;
    const Type* type = nullptr;
    bool isLet = false;      // immutable binding
    bool isWeak = false;
    bool isVar = false;      // declared with `var`
    // Ownership: set when the value is moved out; further use is an error.
    bool moved = false;
    // Definition site for diagnostics.
    Node* decl = nullptr;
    const TypeRecord* record = nullptr;  // for Type / EnumCase symbols
    FunctionDecl* function = nullptr;    // for Function symbols
};

// ─── Sema ──────────────────────────────────────────────────────────────────
class Sema {
public:
    explicit Sema(DiagnosticEngine& diags) : diags_(diags) {}

    // Analyze a whole translation unit (top-level declarations).
    void analyze(const NodeList& decls);

    TypeContext& types() { return types_; }
    const TypeContext& types() const { return types_; }
    // Access a collected type record by name (may be null).
    const TypeRecord* findType(const std::string& name) const;
    // Number of hard errors reported (mirrors diags error count at entry).
    bool hadError() const { return hadError_; }

    // ── Exposed for the driver / tests ────────────────────────────────────
    const std::unordered_map<std::string, TypeRecord*>& allTypes() const { return typeIndex_; }

private:
    // ── collection ──────────────────────────────────────────────────────────
    // Seed the global scope with runtime/stdlib primitives (print, ...) so
    // user code can call them before the bootstrap stdlib is compiled in.
    void registerBuiltins();
    void collectTypeDecl(Node* decl);
    void collectMembers(TypeRecord& rec, TypeDecl* td);
    // Add a single member node to a record (extracted so extensions and
    // protocol defaults can reuse the same logic without re-collecting).
    void addMember(TypeRecord& rec, Node* m);
    // Fold extension members / conformances and inherited protocol defaults
    // into the extended type's member list (规范 2.6 / 4.5).
    void mergeExtensions(const NodeList& decls);
    void mergeDefaultImplementations();
    void checkOverrides();
    void collectTypeAlias(TypealiasDecl* ta);
    // Warn about strong-reference cycles between classes; see the definition
    // for why this reports rather than rewrites.
    void checkReferenceCycles();
    void collectFunction(FunctionDecl* fn, const TypeRecord* owner);
    void collectGlobalVar(VarDecl* vd);

    // Verify every declared protocol conformance is satisfied. A requirement is
    // met by a matching member on the type (or a superclass); protocol
    // requirements that carry a default implementation are exempt.
    void checkConformances();

    // 校验 required/convenience 构造器语义（规范 4.4）。
    void checkInitRules();
    // 成员访问的访问控制检查（规范 10.1）：`private` 成员仅在本类型内可见。
    void checkMemberAccess(const TypeRecord* owner, const TypeRecord::Member* m, Node* at);
    // 泛型约束检查（规范 2.1）：依据 bindings(类型形参→实参) 校验约束列表。
    bool typeConformsTo(const Type* t, const std::string& protoName) const;
    void checkGenericConstraints(const std::vector<GenericConstraint>& cs,
                                 const std::unordered_map<std::string, const Type*>& bindings,
                                 Node* at);

    // ── type resolution (TypeRepr → semantic Type) ──────────────────────────
    const Type* resolveTypeRepr(Node* repr, const TypeRecord* context);
    // Body of resolveTypeRepr; the wrapper caches the result on the node.
    const Type* resolveTypeReprUncached(Node* repr, const TypeRecord* context);
    const Type* resolveTypeReprNoContext(Node* repr);

    // ── body checking ───────────────────────────────────────────────────────
    void checkFunctionBody(FunctionDecl* fn, const TypeRecord* owner);
    void checkGlobalVarBody(VarDecl* vd);
    void checkStatements(const NodeList& stmts, const TypeRecord* context,
                         const Type* fnReturnType, bool isThrowing);
    void checkStatement(Node* stmt, const TypeRecord* context,
                        const Type* fnReturnType, bool isThrowing);

    // Infer the semantic type of an expression (null on error). Records the
    // result on the node (`semaType`) so the code generator can consume it
    // without re-running inference.
    const Type* checkExpr(Node* expr, const TypeRecord* context);
    // Actual inference body; `checkExpr` wraps it to annotate the node.
    const Type* checkExprInner(Node* expr, const TypeRecord* context);
    const Type* checkExprNoContext(Node* expr) { return checkExpr(expr, nullptr); }

    // Helper: require two types compatible; report on mismatch.
    void requireAssignable(const Type* to, const Type* from, Node* at,
                           const std::string& contextDesc);
    // Assignability relation used for initializers, returns, arguments and
    // assignments. Unknown is compatible with everything (error recovery).
    bool isAssignable(const Type* to, const Type* from);
    // Numeric widening helpers (e.g. Int -> Double, Int32 -> Int64).
    bool isWideningNumeric(const Type* to, const Type* from) const;
    // Report "cannot find X in scope" and return Unknown.
    const Type* reportUnresolved(Node* identExpr, const std::string& name);

    // ── control flow ────────────────────────────────────────────────────────
    // True if the statement definitely transfers control (return/break/…).
    bool alwaysTransfers(Node* stmt);
    bool blockAlwaysTransfers(const NodeList& stmts);

    // ── ownership ───────────────────────────────────────────────────────────
    void markMoved(Node* nameExpr, const std::string& name);
    void checkNotMoved(Node* nameExpr, const std::string& name);

    // ── members / lookup ────────────────────────────────────────────────────
    const TypeRecord::Member* lookupMember(const TypeRecord* rec, const std::string& name,
                                           bool wantFunction) const;
    // Find a member function by name and arity.
    const TypeRecord::Member* lookupMethod(const TypeRecord* rec, const std::string& name,
                                           size_t arity) const;
    // Resolve `.member` on a value of type `t` to a semantic type.
    const Type* resolveMemberType(const Type* t, const std::string& name, Node* at,
                                  const TypeRecord* context);
    // Resolve a called function to a function type.
    const Type* resolveCallType(const Type* calleeType, Node* at,
                                const TypeRecord* context, size_t argCount);

    // Trait's name resolution for a type: does `rec` conform to `proto`?
    bool conformsTo(const TypeRecord* rec, const TypeRecord* proto) const;

    DiagnosticEngine& diags_;
    TypeContext types_;

    // Global type index: name → record.
    std::unordered_map<std::string, TypeRecord*> typeIndex_;
    // `typealias ID = Int` maps the alias name to the type it stands for, so
    // resolveTypeRepr can expand it. Aliases are transparent: an alias and its
    // underlying type are the same type, never a distinct one.
    std::unordered_map<std::string, const Type*> typeAliases_;
    // Owns every TypeRecord created for the session.
    std::vector<std::unique_ptr<TypeRecord>> ownedRecords_;
    // Global value/function scope.
    ScopedTable<Symbol> globals_;
    // Per-function local scope stack (parameters + locals).
    ScopedTable<Symbol> locals_;
    // Current function context (for return-type checking while walking).
    const Type* currentReturn_ = nullptr;
    const TypeRecord* currentType_ = nullptr;  // enclosing named type (for `self`)
    bool currentThrows_ = false;
    // While checking a binding/case pattern, bare identifiers are bindings (not
    // value references) and must not be reported as unresolved.
    bool inPattern_ = false;
    // Set while checking an expression against a `Char`-typed context, where a
    // single-scalar string literal denotes a character rather than a String.
    bool charContext_ = false;
    // `let x: Int` without an initialiser: the binding exists but holds no value
    // yet. The first assignment initialises it (规范 1.3 允许延迟初始化); any
    // further assignment is an error, and reading it before initialisation is
    // an error too. Keyed by variable name in the current function.
    std::unordered_set<std::string> pendingInit_;
    // Type of the switch subject while its case clauses are being checked, so
    // that `case let v` can bind `v` with the subject's type.
    const Type* switchSubjectType_ = nullptr;
    // True while walking the body of a switch/select case clause; `fallthrough`
    // is only allowed there.
    bool inCaseBody_ = false;
    bool hadError_ = false;
    // 进入 `unsafe` 块时置位（规范 8.6）：裸指针/非托管类型仅在此上下文可用。
    bool unsafeContext_ = false;
    // Current generic parameter names in scope (for constraints / type names).
    std::vector<std::string> genericParams_;
    // Concrete type for each in-scope type parameter of the instantiation being
    // checked. Empty for the generic body itself.
    std::unordered_map<std::string, const Type*> genericBindings_;
    // Saved bindings while an instance is bound, so nesting is safe.
    std::vector<std::unordered_map<std::string, const Type*>> bindingStack_;
    // Generic instantiations discovered while checking: the generic function and
    // the type arguments each call site supplied. Code generation monomorphises
    // one concrete function per distinct tuple, which is what lets a generic body
    // be lowered against real types instead of a placeholder.
public:
    struct GenericInstance {
        std::string funcName;
        std::vector<std::string> typeArgs;  // printed type names, as written
    };
    // Monomorphised instances discovered while checking, in discovery order.
    const std::vector<GenericInstance>& genericInstances() const {
        return genericInstances_;
    }
    // Re-check `fn` as the `index`-th recorded instantiation, annotating its
    // body with that instance's concrete types. Code generation calls this once
    // per instance, immediately before lowering, so the shared AST carries the
    // types of whichever instance is being emitted.
    void checkInstance(size_t index, FunctionDecl* fn);

    // Hold `instance` bound as the current generic context. The generator binds
    // before declaring *and* emitting an instance body, because both need the
    // concrete types; `unbindInstance` restores the previous state.
    void bindInstance(size_t index, FunctionDecl* fn);
    void unbindInstance();

private:
    std::vector<GenericInstance> genericInstances_;

    // Record `funcName<typeArgs...>` once; duplicates are dropped so the set of
    // monomorphised functions stays minimal.
    // Check every recorded instantiation with its type arguments bound.
    void monomorphise(const NodeList& decls);
    // The type a generic argument is inferred as: a container is matched by its
    // element, since `T` in `[T]` binds to the element, not the array.
    // Check `e` where a `Char` is expected, so a one-scalar string literal is
    // read as a character rather than a String.
    const Type* checkAsChar(Node* e, const TypeRecord* context);
    const Type* inferTypeArgument(const Type* t);
    // Resolve a written type name the way an annotation would.
    const Type* typeByName(const std::string& name);
    void recordGenericInstance(const std::string& funcName,
                               const std::vector<std::string>& typeArgs);
};

} // namespace suki
