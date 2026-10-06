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

#include <functional>
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
    bool isCEnum = false;                        // @enum(C)：C 兼容整数枚举（规范 1.5）
    bool isStdlib = false;                       // 来自被导入的标准库（受信任，可定义 unsafe 类型）
    const TypeRecord* origRec = nullptr;         // 泛型实例记录回指原始声明（枚举穷举比较用）
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
        // 声明所属模块（空 = 用户主模块）。跨模块访问仅 public/open 可见。
        std::string module;
    };
    std::vector<Member> members;
    // Enum cases (name → associated value types).
    struct EnumCaseInfo {
        std::string name;
        std::vector<const Type*> associated;
        int64_t rawValue = -1;           // @enum(C) 显式/顺序原始值（规范 1.5）
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
    // 访问控制（规范 §10.1）：access 为声明的访问级别，module 为其所属模块
    // （空 = 用户主模块）。跨模块访问仅 public/open 可见。
    AccessLevel access = AccessLevel::Internal;
    std::string module;
};

// ─── Sema ──────────────────────────────────────────────────────────────────
class Sema {
public:
    explicit Sema(DiagnosticEngine& diags) : diags_(diags) {}

    // Analyze a whole translation unit (top-level declarations).
    void analyze(NodeList& decls);

    // 宏展开（规范 5.6）：在类型解析之前就地展开 freestanding 宏。
    void expandMacros(NodeList& decls);
    void setSource(const std::string* src) { source_ = src; }

    // 标记为来自被导入标准库（prelude）的声明数量，使其在 unsafe 上下文约束下
    // 仍可定义 unsafe 类型（见 Sema.cpp analyze()）。
    void setStdlibDeclCount(size_t n) { stdlibDeclCount_ = n; }

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
    void collectTypeDecl(Node* decl, bool isStdlib = false);
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
    // 访问控制（规范 §10.1）：跨模块仅 public/open 可见，同模块 internal/
    // fileprivate 可见（private 由所属类型另行判定）。
    bool isAccessible(const std::string& targetModule, AccessLevel access) const;
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
    // 整数字面量范围校验（规范 §1.5）：按后缀宽度（i8…u64，默认 Int=i64）检查
    // 溢出；超界或 ERANGE 时报错，避免静默饱和成错误值（非生产级安全/规范合规）。
    void checkIntegerLiteral(Node* e);
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

    // ── 宏展开（规范 5.6） ──────────────────────────────────────────────────
    void collectMacro(MacroDecl* md);
    bool extractTemplate(MacroDecl* md);
    NodePtr tryExpandMacroCall(CallExpr* call, const std::string& macroName);
    void expandInNode(NodePtr& n);
    void walkChildren(Node* n, std::function<void(NodePtr&)> fn);
    // AST→源码递归序列化：把宏实参（unquote 表达式）还原为源码文本，
    // 避免依赖零长度的 SourceRange 做区间截取（Parser 当前节点 range 均为单点）。
    std::string exprToSource(Node* n) const;
    std::string typeToSource(Node* n) const;
    // unquote 实参若本身含运算符（二元/一元/三元/区间/as/is/赋值等），
    // 嵌入模板后会被周围运算符抢占优先级，需整体加括号（如 (3 + 4)）。
    bool macroArgNeedsParens_(Node* n) const;
    // 由模板 + 实参构造展开源码字符串（M1 表达式宏与 M2 声明宏共用）。
    // selfType 非空时允许模板内 \(Self) 引用被注解类型的名字（@attached 用）。
    std::string buildMacroCode(MacroDecl* md, CallExpr* call, const std::string& selfType);
    // 声明位宏：把 #makeDecl 模板重解析为声明列表。selfType 供 @attached 透传。
    NodeList tryExpandDeclMacro(CallExpr* call, const std::string& selfType);
    // 由模板源码重新解析为声明列表（freestanding(declaration) 与 @attached(member) 共用）。
    NodeList expandDeclTemplate(MacroDecl* md, CallExpr* call, const std::string& selfType);
    // @attached(member) 宏：被注解类型无显式实参，用合成空 CallExpr 触发模板展开。
    NodeList tryExpandAttached(MacroDecl* md, const std::string& selfType);
    // 列表级展开：把列表里「#name(...)」形式的声明位宏调用（以 ExprStmt 承载）
    // 就地替换为展开得到的声明列表，并递归处理嵌套的语句块/类型成员。
    void expandDeclList(NodeList& list);
    // 对单个节点的「含声明列表」子容器递归调用 expandDeclList。
    void expandDeclListContainers(Node* n);
    // 判断节点是否为声明位宏调用（ExprStmt -> CallExpr(#name) 且 name 指向
    // 已注册的 freestanding(declaration) 宏）。
    bool isDeclMacroCall(Node* n) const;
    size_t countNodes(Node* n);
    std::unordered_map<std::string, MacroDecl*> macros_;
    const std::string* source_ = nullptr;   // 调用点源码，unquote 取原始文本用
    uint64_t uniqueCounter_ = 0;
    uint32_t expandDepth_ = 0;
    long expandBudget_ = 0;   // MacroExpansionTooComplex 复杂度预算（节点数）
    static const long kMaxMacroComplexity_ = 200000;

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
    // 当前正在分析的声明所属模块（空 = 用户主模块），用于 §10.1 跨模块访问控制。
    std::string currentModule_;
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
    // 正在检查赋值表达式的左值：此处是「写入」而非「读取」，确定性赋值分析
    // （规范 §1.3）不得把左值当作「使用前未初始化」来报错。
    bool inAssignmentLhs_ = false;
    // Type of the switch subject while its case clauses are being checked, so
    // that `case let v` can bind `v` with the subject's type.
    const Type* switchSubjectType_ = nullptr;
    // True while walking the body of a switch/select case clause; `fallthrough`
    // is only allowed there.
    bool inCaseBody_ = false;
    bool hadError_ = false;
    // 进入 `unsafe` 块时置位（规范 8.6）：裸指针/非托管类型仅在此上下文可用。
    bool unsafeContext_ = false;
    // 规范 §8.7 裸机属性（`never` 之外）：校验结果供 codegen 取用。
    FunctionDecl* panicHandlerFn_ = nullptr;   // @panic_handler 标注的函数
    TypeDecl* globalAllocatorDecl_ = nullptr;  // @global_allocator 标注的类型
    void validateBareMetalAttrs(const NodeList& decls);

    // 正在检查受信任标准库声明时为 true。标准库可自由定义/使用 unsafe 类型
    // （如 memory 模块的 UnsafePointer 系列），故 gating 在 stdlib 上下文跳过，
    // 仅对用户代码（且不在 unsafe 块内）强制（规范 8.6）。
    bool checkingStdlib_ = false;
    // 当前作用域内可见的循环标签（规范 §1.7 `outer: loop { … break outer }`）。
    std::vector<std::string> loopLabels_;
    // 被导入标准库（prelude）声明的数量。这些声明受信任，可定义 unsafe 类型；
    // 用户代码仍受 unsafe 上下文约束。见 analyze() 中各 pass 的按索引切换。
    size_t stdlibDeclCount_ = 0;
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

    // ─── Generic *type* instantiation (规范 5.2) ────────────────────────────
    // `Box<Int>` receives its own TypeRecord with the type parameters substituted,
    // so its fields and methods lower against concrete types rather than the
    // opaque ones a single shared record would imply.
    struct GenericTypeInstance {
        std::string typeName;               // "Box"
        std::string key;                    // "Box<Int>"
        std::vector<std::string> typeArgs;  // printed type arguments
        std::vector<std::string> params;    // generic parameter names, e.g. "T"
        std::vector<const Type*> args;      // concrete argument types
        const Type* instanceType = nullptr; // Named type over the monomorphised record
        bool isClass = false;
    };
    const std::vector<GenericTypeInstance>& genericTypeInstances() const {
        return genericTypeInstances_;
    }
    // Bind / unbind one instantiation's parameters around code generation, so
    // member signatures and bodies are annotated for that instance.
    void bindTypeParams(const std::vector<std::string>& params,
                        const std::vector<const Type*>& args);
    void unbindTypeParams();
    // Re-resolve a function's parameter and return types under the current
    // bindings and re-check its body, so lowering sees this instance's types.
    void resolveFunctionSignature(FunctionDecl* fn);

private:
    std::vector<GenericInstance> genericInstances_;
    std::vector<GenericTypeInstance> genericTypeInstances_;
    // Owning storage for monomorphised records; Named types reference these, and
    // they must outlive the compilation session.
    std::vector<std::unique_ptr<TypeRecord>> monoRecords_;
    const Type* monomorphiseGenericType(const TypeRecord* rec,
                                        const std::string& name,
                                        const std::vector<const Type*>& args);

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
    // 不透明返回类型（规范 5.5）：`func f() -> some P` 的底层具体类型由函数体
    // 推断后原地替换函数类型对象的 ret 字段。此处记录 fn → 推断出的具体类型，
    // 供 checkFunctionBody 取返回类型与调用点一致。
    std::unordered_map<FunctionDecl*, const Type*> opaqueReturnType_;
    // Pass 5 前：对所有返回 `some P` 的函数推断底层具体类型并改写其 ret 字段，
    // 使后续调用点（即使是前向引用）也能拿到具体类型。
    void inferOpaqueReturnTypes(const NodeList& decls);
    const Type* inferOpaqueReturnType(FunctionDecl* fn, const TypeRecord* owner);
    // Resolve a written type name the way an annotation would.
    const Type* typeByName(const std::string& name);
    void recordGenericInstance(const std::string& funcName,
                               const std::vector<std::string>& typeArgs);
};

} // namespace suki
