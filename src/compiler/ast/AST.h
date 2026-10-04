#pragma once

#include "compiler/diag/Diagnostic.h"
#include "compiler/lexer/Token.h"
#include <memory>
#include <string>
#include <vector>

namespace suki {

// ─── AST overview ─────────────────────────────────────────────────────────
// A single base class `Node` with a `NodeKind` discriminator. The compiler is
// built with -fno-rtti (matching LLVM), so we never use dynamic_cast/typeid;
// downcasts are done via static_cast<NodeSub*>(n.get()) after checking kind.
// Every node owns its children through std::unique_ptr<Node>.

using NodePtr = std::unique_ptr<class Node>;
using NodeList = std::vector<NodePtr>;

// Forward-declared semantic type (defined in sema/Type.h). Keeping it opaque
// here preserves the layering: the front end does not depend on the analyser.
struct Type;

// 访问控制级别（规范 10.1）。未显式标注时默认为 Internal。
enum class AccessLevel { Public, Internal, Fileprivate, Private };

enum class NodeKind {
    // Declarations
    ModuleDecl, ImportDecl,
    FunctionDecl, StructDecl, EnumDecl, ClassDecl, ProtocolDecl, ExtensionDecl,
    InitDecl, DeinitDecl, SubscriptDecl,
    VarDecl, AccessorDecl, TypealiasDecl, EnumCaseDecl, AssociatedTypeDecl, MacroDecl,
    ActorDecl,
    // Statements
    BlockStmt, ExprStmt, ReturnStmt, IfStmt, GuardStmt, WhileStmt,
    RepeatWhileStmt, ForInStmt, SwitchStmt, CaseClause, BreakStmt,
    ContinueStmt, DeferStmt, DoStmt, CatchClause, ThrowStmt, UnsafeStmt,
    // `fallthrough` — 显式落入下一个 case（规范 1.7 保留的关键字）。
    FallthroughStmt,
    // Expressions
    IdentExpr, IntLitExpr, FloatLitExpr, StrLitExpr, CharLitExpr, BoolLitExpr,
    BinaryExpr, UnaryExpr, CallExpr, MemberExpr, SubscriptExpr,
    OptionalChainExpr, ForceUnwrapExpr, TupleExpr, ArrayLitExpr, DictLitExpr,
    SetLitExpr, AsmExpr,
    ClosureExpr, ParenExpr, AsExpr, IsExpr, AssignmentExpr, RangeExpr,
    NilLitExpr, GenericExpr, MoveExpr, IfExpr, TernaryExpr,
    // Type representations
    NamedType, OptionalType, ArrayType, DictType, TupleType, FuncType,
    RefType, InoutType, NeverType, PlaceholderType, MetatypeType,
};

struct Node {
    NodeKind kind;
    SourceRange range;
    std::string docComment;
    std::vector<std::string> attributes; // e.g. "main" from @main

    // Semantic type resolved by Sema and consumed by the code generator. This
    // is the single source of truth for lowering: IRGenerator prefers it and
    // only falls back to syntactic inference when it is absent, which keeps
    // older code paths working while the analyser annotates more nodes.
    const Type* semaType = nullptr;

    // `try` / `await` are prefix keywords, not operators. They are recorded as
    // dedicated flags instead of being encoded in `UnaryExpr::op` as a marker,
    // which used to overload `!` / `~` and created real ambiguity.
    bool isTry = false;
    bool isAwait = false;
    // Set on the function body node of an `async` function.
    bool isAsync = false;

    explicit Node(NodeKind k) : kind(k) {}
    virtual ~Node() = default;
};

// ─── Parameter & pattern helpers ───────────────────────────────────────────
struct Param {
    std::string externalName; // argument label ("_" means no label)
    std::string internalName;
    NodePtr type;             // TypeRepr or null
    NodePtr defaultValue;     // Expr or null
    bool isInout = false;
    bool isVariadic = false; // `T...`
    // `var` 前缀参数（规范 3.1）：函数内部可修改形参副本，不影响调用者。
    // 与 `inout` 相反，inout 通过 & 传参并直接改写调用者的变量。
    bool isVar = false;
    // Parameter type resolved by Sema; authoritative for lowering.
    const Type* semaType = nullptr;
    SourceRange range;
};

// ─── Declarations ──────────────────────────────────────────────────────────
// 泛型约束（规范 2.1）：`func f<T: Equatable>(...)` 或 `where T: Equatable, U == V`。
// lhs 为类型形参（或类型），rhs 为协议名（约束）或类型（same-type）；sameType
// 区分 `T: Proto`（遵循协议）与 `T == U`（同类型约束）。
struct GenericConstraint {
    GenericConstraint() {}
    GenericConstraint(NodePtr l, NodePtr r, bool st)
        : lhs(std::move(l)), rhs(std::move(r)), sameType(st) {}
    NodePtr lhs;
    NodePtr rhs;
    bool sameType = false;
};

struct TypeDecl : Node {
    TypeDecl(NodeKind k) : Node(k) {}
    std::string name;
    std::vector<std::string> modifiers;     // public/static/mutating/...
    std::vector<std::string> genericParams; // T, E, ...
    NodeList inherited;                     // base types / protocols
    NodeList members;
    NodePtr whereClause;                    // optional `where` clause (Expr)
    std::vector<GenericConstraint> genericConstraints; // 泛型约束（规范 2.1）
    bool isCEnum = false;               // @enum(C)：C 兼容整数枚举（规范 1.5）
    // 宿主类型名（类型嵌套，规范 4.6）。`Outer.Inner` 的成员查找靠它把
    // 子类型登记到外层类型上；顶层类型为空。
    std::string enclosingType;
};

struct ModuleDecl : Node {
    ModuleDecl() : Node(NodeKind::ModuleDecl) {}
    std::string name;
};

struct ImportDecl : Node {
    ImportDecl() : Node(NodeKind::ImportDecl) {}
    std::string moduleName;
};

struct VarDecl : Node {
    VarDecl() : Node(NodeKind::VarDecl) {}
    bool isLet = true;
    std::string name;
    // 元组解构绑定：`let (a, b) = (1, 2)`。当 size() > 1 时 names[0] 即 name，
    // 元素类型依次为初始化元组的各分量类型。
    std::vector<std::string> tupleNames;
    NodePtr type;            // TypeRepr or null
    NodePtr initializer;     // Expr or null
    std::vector<std::string> modifiers;
    bool isWeak = false;
    // `unowned` also breaks the retain cycle, but reading it asserts non-nil
    // instead of yielding nil.
    bool isUnowned = false;
    bool isMove = false;
    // Structured accessors of a computed property. Kept separate from
    // `accessorBody` (the statements of an implicit getter) because each kind
    // lowers differently: a getter is a function returning the property type,
    // a setter takes the new value, and the observers fire around an assignment.
    std::vector<NodePtr> accessors;
};

struct FunctionDecl : Node {
    FunctionDecl() : Node(NodeKind::FunctionDecl) {}
    std::string name;
    std::vector<std::string> modifiers;
    bool isAsync = false;
    bool isMutating = false;
    bool isThrows = false;
    std::vector<Param> params;
    NodePtr returnType;      // TypeRepr or null
    NodeList body;           // statements; empty if external (`foreign`)
    bool isForeign = false;
    std::string cdeclName;                         // @_cdecl("name")：外部符号名（P6.3）
    // True for a `func` in a protocol whose body supplies the default
    // implementation (规范 4.5). Such a member is lowered into every conforming
    // type but must not be type-checked again there.
    bool isDefaultImpl = false;
    std::vector<std::string> genericParams;
    NodePtr whereClause;
    std::vector<GenericConstraint> genericConstraints; // 泛型约束（规范 2.1）
};

struct InitDecl : Node {
    InitDecl() : Node(NodeKind::InitDecl) {}
    std::vector<std::string> modifiers;
    std::vector<Param> params;
    NodeList body;
    bool isConvenience = false;
    bool isRequired = false;
};

struct DeinitDecl : Node {
    DeinitDecl() : Node(NodeKind::DeinitDecl) {}
    NodeList body;
};

struct SubscriptDecl : Node {
    SubscriptDecl() : Node(NodeKind::SubscriptDecl) {}
    std::vector<std::string> modifiers;
    std::vector<Param> params;
    NodePtr elementType;
    NodeList getter;   // get { ... }
    NodeList setter;   // set { ... } (optional)
};

struct TypealiasDecl : Node {
    TypealiasDecl() : Node(NodeKind::TypealiasDecl) {}
    std::vector<std::string> modifiers;
    std::string name;
    std::vector<std::string> genericParams;
    NodePtr underlying; // TypeRepr
    std::vector<GenericConstraint> genericConstraints; // 泛型约束（规范 2.1）
};

struct EnumCaseDecl : Node {
    EnumCaseDecl() : Node(NodeKind::EnumCaseDecl) {}
    std::vector<std::string> modifiers;
    std::string name;
    std::vector<NodePtr> associatedTypes; // TypeRepr list (if any)
    bool hasAssociated = false;
    long long rawValue = -1;             // 显式原始值（@enum(C)，规范 1.5）；-1 表示自动顺序赋值
};

// `get { }`, `set(v) { }`, `willSet { }`, `didSet { }`
struct AccessorDecl : Node {
    AccessorDecl() : Node(NodeKind::AccessorDecl) {}
    enum class Kind { Getter, Setter, WillSet, DidSet };
    Kind kind = Kind::Getter;
    std::string valueParam;  // setter's parameter name, e.g. `set(newValue)`
    NodeList body;
};

struct AssociatedTypeDecl : Node {
    AssociatedTypeDecl() : Node(NodeKind::AssociatedTypeDecl) {}
    std::vector<std::string> modifiers;
    std::string name;
    NodeList inherited;
    NodePtr defaultType;
};

struct MacroDecl : Node {
    MacroDecl() : Node(NodeKind::MacroDecl) {}
    std::string name;
    std::vector<std::string> params;
};

struct StructDecl : TypeDecl { StructDecl() : TypeDecl(NodeKind::StructDecl) {} };
struct EnumDecl   : TypeDecl { EnumDecl()   : TypeDecl(NodeKind::EnumDecl) {} };
struct ClassDecl  : TypeDecl { ClassDecl()  : TypeDecl(NodeKind::ClassDecl) {} };
struct ActorDecl  : TypeDecl { ActorDecl()  : TypeDecl(NodeKind::ActorDecl) {} };
struct ProtocolDecl : TypeDecl { ProtocolDecl() : TypeDecl(NodeKind::ProtocolDecl) {} };
struct ExtensionDecl : TypeDecl { ExtensionDecl() : TypeDecl(NodeKind::ExtensionDecl) {} };

// ─── Statements ────────────────────────────────────────────────────────────
struct BlockStmt : Node {
    BlockStmt() : Node(NodeKind::BlockStmt) {}
    NodeList statements;
};

struct ExprStmt : Node {
    ExprStmt() : Node(NodeKind::ExprStmt) {}
    NodePtr expr;
};

struct ReturnStmt : Node {
    ReturnStmt() : Node(NodeKind::ReturnStmt) {}
    NodePtr value; // may be null
};

struct IfStmt : Node {
    IfStmt() : Node(NodeKind::IfStmt) {}
    NodePtr condition;
    NodeList thenBody;
    NodePtr elseBranch; // BlockStmt or IfStmt or null
};

struct GuardStmt : Node {
    GuardStmt() : Node(NodeKind::GuardStmt) {}
    NodePtr condition;
    NodeList elseBody; // must transfer control
};

struct WhileStmt : Node {
    WhileStmt() : Node(NodeKind::WhileStmt) {}
    NodePtr condition;
    NodeList body;
};

struct RepeatWhileStmt : Node {
    RepeatWhileStmt() : Node(NodeKind::RepeatWhileStmt) {}
    NodeList body;
    NodePtr condition;
};

struct ForInStmt : Node {
    ForInStmt() : Node(NodeKind::ForInStmt) {}
    NodePtr pattern;  // VarDecl or pattern node
    NodePtr sequence; // Expr
    NodeList body;
    bool isAsync = false; // true for `for await ... in`
};

struct SwitchStmt : Node {
    SwitchStmt() : Node(NodeKind::SwitchStmt) {}
    NodePtr subject;
    std::vector<NodePtr> cases; // CaseClause
};

struct CaseClause : Node {
    CaseClause() : Node(NodeKind::CaseClause) {}
    bool isDefault = false;
    NodePtr pattern;  // Expr pattern
    // Names bound by an enum-case payload pattern, in payload order:
    // `case Shape.circle(let r)` binds "r"; `case Shape.rect(let w, let h)`
    // binds "w" and "h". Empty for patterns without a binding list.
    std::vector<std::string> bindings;
    // Additional patterns for the same arm (`case 1, 2:`). Any of them selects
    // this body; they carry no body of their own.
    std::vector<NodePtr> alternatives;
    // `case let v` / `case var v` —— 值绑定模式：匹配任意值并把 subject 绑定
    // 到 bindings 里的名字（可再由 where 过滤）。与枚举负载绑定不同，这里
    // pattern 本身不参与相等比较。
    bool isBindingPattern = false;
    NodePtr whereExpr; // optional `where`
    NodeList body;
};

struct BreakStmt : Node {
    BreakStmt() : Node(NodeKind::BreakStmt) {}
    std::string label;
};

// `fallthrough` —— 显式落入下一个 case（规范 1.7 保留的关键字）。
// 与之相对，switch 的其他 case 默认不穿透。
struct FallthroughStmt : Node {
    FallthroughStmt() : Node(NodeKind::FallthroughStmt) {}
};

struct ContinueStmt : Node {
    ContinueStmt() : Node(NodeKind::ContinueStmt) {}
    std::string label;
};

struct DeferStmt : Node {
    DeferStmt() : Node(NodeKind::DeferStmt) {}
    NodePtr body; // single statement (usually a CallExpr)
};

struct DoStmt : Node {
    DoStmt() : Node(NodeKind::DoStmt) {}
    NodeList body;
    std::vector<NodePtr> catches; // CatchClause
};

struct CatchClause : Node {
    CatchClause() : Node(NodeKind::CatchClause) {}
    NodePtr pattern;  // optional binding pattern
    NodePtr whereExpr; // optional `where`
    NodeList body;
};

struct ThrowStmt : Node {
    ThrowStmt() : Node(NodeKind::ThrowStmt) {}
    NodePtr value;
};

struct UnsafeStmt : Node {
    UnsafeStmt() : Node(NodeKind::UnsafeStmt) {}
    NodeList body;
};

// 内联汇编（规范 8.2）：LLVM 风格 `asm("..." : outputs : inputs : clobbers)`，
// 必须在 unsafe 块内。
struct AsmOperand {
    std::string constraint;   // 如 "=r"、"r"、"m"、"i"
    NodePtr expr;             // 输出/输入操作数表达式（输出通常是变量）
};
struct AsmExpr : Node {
    AsmExpr() : Node(NodeKind::AsmExpr) {}
    std::string templateStr;                 // 汇编模板
    std::vector<AsmOperand> outputs;         // ": ..." 输出操作数
    std::vector<AsmOperand> inputs;          // ": ..." 输入操作数
    std::vector<std::string> clobbers;       // ": ..." 破坏寄存器（"cc"/"memory"）
};

// ─── Expressions ─────────────────────────────────────────────────────────────
struct IdentExpr : Node {
    IdentExpr() : Node(NodeKind::IdentExpr) {}
    std::string name;
};

struct IntLitExpr : Node {
    IntLitExpr() : Node(NodeKind::IntLitExpr) {}
    std::string value; // original text, suffix included
};

struct FloatLitExpr : Node {
    FloatLitExpr() : Node(NodeKind::FloatLitExpr) {}
    std::string value;
};

struct StrLitExpr : Node {
    StrLitExpr() : Node(NodeKind::StrLitExpr) {}
    std::vector<std::string> segments;   // text fragments
    std::vector<NodePtr> expressions;     // interpolated expressions
    bool isRaw = false;
    bool isMultiline = false;
};

struct CharLitExpr : Node {
    CharLitExpr() : Node(NodeKind::CharLitExpr) {}
    std::string value;
};

struct BoolLitExpr : Node {
    BoolLitExpr() : Node(NodeKind::BoolLitExpr) {}
    bool value = false;
};

struct BinaryExpr : Node {
    BinaryExpr() : Node(NodeKind::BinaryExpr) {}
    PunctuatorID op = PunctuatorID::None;
    NodePtr lhs;
    NodePtr rhs;
    bool isAssignment = false; // true when op == Equal
};

struct UnaryExpr : Node {
    UnaryExpr() : Node(NodeKind::UnaryExpr) {}
    PunctuatorID op = PunctuatorID::None;
    NodePtr operand;
    bool isPostfix = false;
    // `try?` / `try!` (规范 9.2): the marker is part of the prefix operator,
    // not a postfix `?`/`!` on the operand — `try?` yields an Optional and
    // `try!` asserts that no error is thrown.
    bool isOptionalTry = false;
    bool isForcedTry = false;
};

struct CallExpr : Node {
    CallExpr() : Node(NodeKind::CallExpr) {}
    NodePtr callee;
    std::vector<std::string> argumentLabels; // "" for positional
    std::vector<NodePtr> arguments;
    bool hasTrailingClosure = false;
};

struct MemberExpr : Node {
    MemberExpr() : Node(NodeKind::MemberExpr) {}
    NodePtr base;
    std::string member;
    bool optionalChain = false; // base?.member
    bool isMemoryLayoutQuery = false;        // MemoryLayout<T>.size/stride/alignment（规范 P4.5）
    const Type* memoryLayoutType = nullptr;  // 关联类型 T（供 codegen 查布局）
};

struct SubscriptExpr : Node {
    SubscriptExpr() : Node(NodeKind::SubscriptExpr) {}
    NodePtr base;
    NodeList indices;
};

struct OptionalChainExpr : Node {
    OptionalChainExpr() : Node(NodeKind::OptionalChainExpr) {}
    NodePtr expr; // already optional-chained expr
};

struct ForceUnwrapExpr : Node {
    ForceUnwrapExpr() : Node(NodeKind::ForceUnwrapExpr) {}
    NodePtr expr;
};

struct TupleExpr : Node {
    TupleExpr() : Node(NodeKind::TupleExpr) {}
    std::vector<std::string> labels;
    std::vector<NodePtr> elements;
};

// `{1, 2, 3}` — a set literal. A brace is otherwise the start of a closure or
// a block, so this form is only recognised where an expression may begin.
struct SetLitExpr : Node {
    SetLitExpr() : Node(NodeKind::SetLitExpr) {}
    NodeList elements;
};

struct ArrayLitExpr : Node {
    ArrayLitExpr() : Node(NodeKind::ArrayLitExpr) {}
    std::vector<NodePtr> elements;
};

struct DictLitExpr : Node {
    DictLitExpr() : Node(NodeKind::DictLitExpr) {}
    std::vector<NodePtr> keys;
    std::vector<NodePtr> values;
};

// 闭包捕获项：`[weak self]`、`[unowned owner]`、`[x]`、`[weak varName]`。
// Strong 捕获按值拷贝（引用类型触发 retain），Weak/Unowned 打破循环引用。
struct ClosureCapture {
    enum class Mode { Strong, Weak, Unowned };
    Mode mode = Mode::Strong;
    std::string name;      // 被捕获的变量名
    bool isLet = false;    // 捕获列表里写了 `let`/`var` 标记
    SourceRange range;
};

struct ClosureExpr : Node {
    ClosureExpr() : Node(NodeKind::ClosureExpr) {}
    std::vector<Param> params;
    NodePtr returnType;
    NodeList body;
    bool isArrowSyntax = false; // true for `(...) => expr`
    // 显式捕获列表；为空表示按需隐式捕获（引用外层变量）。
    std::vector<ClosureCapture> captures;
    // 编译期内置的自动闭包（`@autoclosure` 形参的实参被包成无参闭包）。
    bool isAutoclosure = false;
};

struct ParenExpr : Node {
    ParenExpr() : Node(NodeKind::ParenExpr) {}
    NodePtr expr;
};

struct AsExpr : Node {
    AsExpr() : Node(NodeKind::AsExpr) {}
    NodePtr expr;
    NodePtr type;
    enum Kind { As, AsQuestion, AsBang } asKind = As;
};

struct IsExpr : Node {
    IsExpr() : Node(NodeKind::IsExpr) {}
    NodePtr expr;
    NodePtr type;
};

struct AssignmentExpr : Node {
    AssignmentExpr() : Node(NodeKind::AssignmentExpr) {}
    NodePtr lhs;
    NodePtr rhs;
    bool isCompound = false;             // true for +=, -=, *=, /=, %=
    PunctuatorID compoundOp = PunctuatorID::None; // the binary op for compound
};

struct RangeExpr : Node {
    RangeExpr() : Node(NodeKind::RangeExpr) {}
    NodePtr lower;
    NodePtr upper;
    bool halfOpen = true; // true: ..< (half-open), false: ... (closed)
};

struct NilLitExpr : Node {
    NilLitExpr() : Node(NodeKind::NilLitExpr) {}
};

struct GenericExpr : Node {
    GenericExpr() : Node(NodeKind::GenericExpr) {}
    NodePtr base;     // IdentExpr or MemberExpr being specialized
    NodeList args;    // type arguments
};

// `cond ? a : b` — the conditional operator.
struct TernaryExpr : Node {
    TernaryExpr() : Node(NodeKind::TernaryExpr) {}
    NodePtr condition;
    NodePtr thenValue;
    NodePtr elseValue;
};

struct MoveExpr : Node {
    MoveExpr() : Node(NodeKind::MoveExpr) {}
    NodePtr operand;  // the value being moved
};

// `if` used as an expression: `let x = if cond { a } else { b }`
struct IfExpr : Node {
    IfExpr() : Node(NodeKind::IfExpr) {}
    NodePtr condition;
    NodeList thenBody;
    NodePtr elseBranch; // BlockStmt / IfExpr / null
};

// ─── Type representations ─────────────────────────────────────────────────────
struct NamedType : Node {
    NamedType() : Node(NodeKind::NamedType) {}
    std::string name;
    std::vector<NodePtr> genericArgs;
};

struct OptionalType : Node {
    OptionalType() : Node(NodeKind::OptionalType) {}
    NodePtr wrapped;
};

struct ArrayType : Node {
    ArrayType() : Node(NodeKind::ArrayType) {}
    NodePtr element;
};

struct DictType : Node {
    DictType() : Node(NodeKind::DictType) {}
    NodePtr key;
    NodePtr value;
};

struct TupleType : Node {
    TupleType() : Node(NodeKind::TupleType) {}
    std::vector<std::string> labels;
    std::vector<NodePtr> elements;
};

struct FuncType : Node {
    FuncType() : Node(NodeKind::FuncType) {}
    std::vector<NodePtr> params;
    NodePtr ret;
};

struct RefType : Node {
    RefType() : Node(NodeKind::RefType) {}
    std::string refKind; // "shared" | "unique" | "mut" | "weak" | "unowned"
    NodePtr pointee;
};

struct InoutType : Node {
    InoutType() : Node(NodeKind::InoutType) {}
    NodePtr pointee;
};

struct NeverType : Node { NeverType() : Node(NodeKind::NeverType) {} };
struct PlaceholderType : Node { PlaceholderType() : Node(NodeKind::PlaceholderType) {} };

struct MetatypeType : Node {
    MetatypeType() : Node(NodeKind::MetatypeType) {}
    NodePtr base;
    bool isMeta = true; // true: T.Type, false: T.Protocol
};

} // namespace suki
