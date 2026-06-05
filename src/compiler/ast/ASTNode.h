#pragma once
// Abstract Syntax Tree node definitions for SukiCode.
// The AST is a rich, location-annotated tree representing parsed source code.

#include "compiler/lexer/Token.h"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace suki {

// Forward declarations
struct Expr;
struct Stmt;
struct Decl;
struct TypeRepr;
struct Pattern;

using ExprPtr = std::unique_ptr<Expr>;
using StmtPtr = std::unique_ptr<Stmt>;
using DeclPtr = std::unique_ptr<Decl>;
using TypeReprPtr = std::unique_ptr<TypeRepr>;
using PatternPtr = std::unique_ptr<Pattern>;

// ─── Node ID for unique identification ────────────────────────────────────
using NodeID = uint64_t;

// ─── Base AST Node ────────────────────────────────────────────────────────
struct ASTNode {
    NodeID id;
    SourceLocation loc;

    ASTNode() : id(0), loc() {}
    virtual ~ASTNode() = default;
};

// ─── Type Representations ─────────────────────────────────────────────────
// These represent type annotations in source, not resolved types.

enum class TypeReprKind : uint8_t {
    Named,        // Int, String, MyStruct
    Array,        // [T]
    Dictionary,   // [K: V]
    Tuple,        // (A, B, C)
    Optional,     // T?
    Function,     // (A, B) -> C
    Composition,  // A & B
    Opaque,       // some P
    Existential,  // any P
    Owned,        // Owned<T>
    Self,         // Self
    Inferred,     // _ (wildcard type)
};

struct TypeRepr : ASTNode {
    TypeReprKind typeReprKind;
    explicit TypeRepr(TypeReprKind k) : typeReprKind(k) {}
};

struct NamedTypeRepr : TypeRepr {
    std::string name;
    std::vector<TypeReprPtr> genericArgs; // e.g., Dictionary<String, Int>
    NamedTypeRepr() : TypeRepr(TypeReprKind::Named) {}
};

struct ArrayTypeRepr : TypeRepr {
    TypeReprPtr elementType;
    ArrayTypeRepr() : TypeRepr(TypeReprKind::Array) {}
};

struct DictTypeRepr : TypeRepr {
    TypeReprPtr keyType;
    TypeReprPtr valueType;
    DictTypeRepr() : TypeRepr(TypeReprKind::Dictionary) {}
};

struct TupleTypeRepr : TypeRepr {
    struct Element {
        std::string label; // optional
        TypeReprPtr type;
    };
    std::vector<Element> elements;
    TupleTypeRepr() : TypeRepr(TypeReprKind::Tuple) {}
};

struct OptionalTypeRepr : TypeRepr {
    TypeReprPtr base;
    OptionalTypeRepr() : TypeRepr(TypeReprKind::Optional) {}
};

struct FunctionTypeRepr : TypeRepr {
    struct Param {
        std::string label; // optional
        TypeReprPtr type;
        bool isInOut = false;
    };
    std::vector<Param> params;
    TypeReprPtr returnType;
    bool isAsync = false;
    bool isThrows = false;
    FunctionTypeRepr() : TypeRepr(TypeReprKind::Function) {}
};

struct CompositionTypeRepr : TypeRepr {
    std::vector<TypeReprPtr> protocols;
    CompositionTypeRepr() : TypeRepr(TypeReprKind::Composition) {}
};

struct OpaqueTypeRepr : TypeRepr {
    TypeReprPtr constraint;
    OpaqueTypeRepr() : TypeRepr(TypeReprKind::Opaque) {}
};

struct ExistentialTypeRepr : TypeRepr {
    TypeReprPtr constraint;
    ExistentialTypeRepr() : TypeRepr(TypeReprKind::Existential) {}
};

struct OwnedTypeRepr : TypeRepr {
    TypeReprPtr inner;
    OwnedTypeRepr() : TypeRepr(TypeReprKind::Owned) {}
};

struct SelfTypeRepr : TypeRepr {
    SelfTypeRepr() : TypeRepr(TypeReprKind::Self) {}
};

// ─── Patterns ─────────────────────────────────────────────────────────────

enum class PatternKind : uint8_t {
    Identifier,   // x, _ (wildcard)
    Tuple,        // (a, b, c)
    EnumCase,     // .success(let x)
    Wildcard,     // _
    Expression,   // constant patterns: 0, "hello"
    IsType,       // is String
    AsType,       // pattern as Type
    Optional,     // let x?
};

struct Pattern : ASTNode {
    PatternKind patternKind;
    explicit Pattern(PatternKind k) : patternKind(k) {}
};

struct IdentifierPattern : Pattern {
    std::string name;
    bool isLet = true; // let vs var binding
    IdentifierPattern() : Pattern(PatternKind::Identifier) {}
};

struct WildcardPattern : Pattern {
    WildcardPattern() : Pattern(PatternKind::Wildcard) {}
};

struct TuplePattern : Pattern {
    std::vector<PatternPtr> elements;
    TuplePattern() : Pattern(PatternKind::Tuple) {}
};

struct EnumCasePattern : Pattern {
    std::string enumName;   // optional: Result.success
    std::string caseName;   // success
    std::vector<PatternPtr> associatedPatterns; // destructured associated values
    EnumCasePattern() : Pattern(PatternKind::EnumCase) {}
};

struct IsTypePattern : Pattern {
    TypeReprPtr type;
    IsTypePattern() : Pattern(PatternKind::IsType) {}
};

struct AsTypePattern : Pattern {
    PatternPtr subPattern;
    TypeReprPtr type;
    AsTypePattern() : Pattern(PatternKind::AsType) {}
};

// ─── Expressions ──────────────────────────────────────────────────────────

enum class ExprKind : uint8_t {
    IntegerLiteral,
    FloatLiteral,
    StringLiteral,
    CharLiteral,
    BoolLiteral,
    NilLiteral,
    Identifier,
    Binary,
    Unary,
    Prefix,          // prefix operators
    Postfix,         // postfix operators
    Call,
    MemberAccess,
    Subscript,
    If,              // if as expression (ternary-like)
    Closure,
    ArrayLiteral,
    DictLiteral,
    SetLiteral,
    Tuple,
    TypeCast,        // as, as?, as!
    TypeCheck,       // is
    OptionalChain,   // ?.
    ForceUnwrap,     // !
    Assignment,
    InOut,           // &variable
    Move,            // move variable
    Await,           // await expr
    Try,             // try expr
    Selector,        // #selector(method)
    SuperRef,        // super.method
    SelfRef,         // self
    InterpolatedString, // "text \(expr) more"
};

struct Expr : ASTNode {
    ExprKind exprKind;
    explicit Expr(ExprKind k) : exprKind(k) {}
};

struct IntegerLiteralExpr : Expr {
    int64_t value;
    IntegerLiteralExpr() : Expr(ExprKind::IntegerLiteral) {}
};

struct FloatLiteralExpr : Expr {
    double value;
    FloatLiteralExpr() : Expr(ExprKind::FloatLiteral) {}
};

struct StringLiteralExpr : Expr {
    std::string value;
    StringLiteralExpr() : Expr(ExprKind::StringLiteral) {}
};

struct CharLiteralExpr : Expr {
    uint32_t value; // Unicode scalar
    CharLiteralExpr() : Expr(ExprKind::CharLiteral) {}
};

struct BoolLiteralExpr : Expr {
    bool value;
    BoolLiteralExpr() : Expr(ExprKind::BoolLiteral) {}
};

struct NilLiteralExpr : Expr {
    NilLiteralExpr() : Expr(ExprKind::NilLiteral) {}
};

struct IdentifierExpr : Expr {
    std::string name;
    IdentifierExpr() : Expr(ExprKind::Identifier) {}
};

struct BinaryExpr : Expr {
    TokenKind op;
    ExprPtr left;
    ExprPtr right;
    BinaryExpr() : Expr(ExprKind::Binary) {}
};

struct UnaryExpr : Expr {
    TokenKind op; // -, !, ~
    ExprPtr operand;
    bool isPrefix = true;
    UnaryExpr() : Expr(ExprKind::Unary) {}
};

struct CallExpr : Expr {
    ExprPtr callee;
    struct Arg {
        std::string label; // optional label
        ExprPtr value;
    };
    std::vector<Arg> args;
    CallExpr() : Expr(ExprKind::Call) {}
};

struct MemberAccessExpr : Expr {
    ExprPtr base;
    std::string member;
    bool isOptionalChain = false; // base?.member
    MemberAccessExpr() : Expr(ExprKind::MemberAccess) {}
};

struct SubscriptExpr : Expr {
    ExprPtr base;
    std::vector<ExprPtr> indices;
    bool isOptionalChain = false;
    SubscriptExpr() : Expr(ExprKind::Subscript) {}
};

struct IfExpr : Expr {
    ExprPtr condition;
    ExprPtr thenExpr;
    ExprPtr elseExpr; // optional
    IfExpr() : Expr(ExprKind::If) {}
};

struct ClosureExpr : Expr {
    struct Param {
        std::string name;
        TypeReprPtr type; // optional
    };
    std::vector<Param> params;
    TypeReprPtr returnType; // optional
    std::vector<StmtPtr> body;
    bool isArrow = false; // true for () => expr, false for { } style
    std::vector<std::string> captureList; // [weak self], [unowned delegate]
    ClosureExpr() : Expr(ExprKind::Closure) {}
};

struct ArrayLiteralExpr : Expr {
    std::vector<ExprPtr> elements;
    ArrayLiteralExpr() : Expr(ExprKind::ArrayLiteral) {}
};

struct DictLiteralExpr : Expr {
    struct Entry {
        ExprPtr key;
        ExprPtr value;
    };
    std::vector<Entry> entries;
    DictLiteralExpr() : Expr(ExprKind::DictLiteral) {}
};

struct SetLiteralExpr : Expr {
    std::vector<ExprPtr> elements;
    SetLiteralExpr() : Expr(ExprKind::SetLiteral) {}
};

struct TupleExpr : Expr {
    struct Element {
        std::string label; // optional
        ExprPtr value;
    };
    std::vector<Element> elements;
    TupleExpr() : Expr(ExprKind::Tuple) {}
};

enum class CastKind : uint8_t {
    Coerce,      // as
    Conditional, // as?
    Force,       // as!
};

struct TypeCastExpr : Expr {
    ExprPtr subExpr;
    TypeReprPtr targetType;
    CastKind castKind;
    TypeCastExpr() : Expr(ExprKind::TypeCast) {}
};

struct TypeCheckExpr : Expr {
    ExprPtr subExpr;
    TypeReprPtr checkType;
    TypeCheckExpr() : Expr(ExprKind::TypeCheck) {}
};

struct OptionalChainExpr : Expr {
    ExprPtr subExpr;
    OptionalChainExpr() : Expr(ExprKind::OptionalChain) {}
};

struct ForceUnwrapExpr : Expr {
    ExprPtr subExpr;
    ForceUnwrapExpr() : Expr(ExprKind::ForceUnwrap) {}
};

struct AssignmentExpr : Expr {
    ExprPtr target;
    ExprPtr value;
    AssignmentExpr() : Expr(ExprKind::Assignment) {}
};

struct InOutExpr : Expr {
    ExprPtr subExpr;
    InOutExpr() : Expr(ExprKind::InOut) {}
};

struct MoveExpr : Expr {
    ExprPtr subExpr;
    MoveExpr() : Expr(ExprKind::Move) {}
};

struct AwaitExpr : Expr {
    ExprPtr subExpr;
    AwaitExpr() : Expr(ExprKind::Await) {}
};

struct TryExpr : Expr {
    ExprPtr subExpr;
    bool isForce = false;  // try!
    bool isOptional = false; // try?
    TryExpr() : Expr(ExprKind::Try) {}
};

struct InterpolatedStringExpr : Expr {
    // "Hello \(name), you are \(age) years old"
    struct Segment {
        std::string literalText; // text before interpolation
        ExprPtr expression;      // the interpolated expression (null for final segment)
    };
    std::vector<Segment> segments;
    InterpolatedStringExpr() : Expr(ExprKind::InterpolatedString) {}
};

struct SuperRefExpr : Expr {
    SuperRefExpr() : Expr(ExprKind::SuperRef) {}
};

struct SelfRefExpr : Expr {
    SelfRefExpr() : Expr(ExprKind::SelfRef) {}
};

// ─── Statements ───────────────────────────────────────────────────────────

enum class StmtKind : uint8_t {
    Expression,
    Return,
    Break,
    Continue,
    Fallthrough,
    Defer,
    Throw,          // throw expr
    VariableDecl,   // let x = ... / var y: T
    DeclStmt,       // control flow decl used as statement (if, for, while, etc.)
    Compound,       // { stmts }
};

struct Stmt : ASTNode {
    StmtKind stmtKind;
    explicit Stmt(StmtKind k) : stmtKind(k) {}
};

struct ExpressionStmt : Stmt {
    ExprPtr expression;
    ExpressionStmt() : Stmt(StmtKind::Expression) {}
};

struct ReturnStmt : Stmt {
    ExprPtr value; // optional
    ReturnStmt() : Stmt(StmtKind::Return) {}
};

struct BreakStmt : Stmt {
    BreakStmt() : Stmt(StmtKind::Break) {}
};

struct ContinueStmt : Stmt {
    ContinueStmt() : Stmt(StmtKind::Continue) {}
};

struct FallthroughStmt : Stmt {
    FallthroughStmt() : Stmt(StmtKind::Fallthrough) {}
};

struct DeferStmt : Stmt {
    std::vector<StmtPtr> body;
    DeferStmt() : Stmt(StmtKind::Defer) {}
};

struct CompoundStmt : Stmt {
    std::vector<StmtPtr> statements;
    CompoundStmt() : Stmt(StmtKind::Compound) {}
};

struct ThrowStmt : Stmt {
    ExprPtr value;
    ThrowStmt() : Stmt(StmtKind::Throw) {}
};

struct VariableDeclStmt : Stmt {
    DeclPtr varDecl;
    VariableDeclStmt() : Stmt(StmtKind::VariableDecl) {}
};

// Generic wrapper: any Decl used as a statement (if, for, while, switch, etc.)
struct DeclStmt : Stmt {
    DeclPtr decl;
    DeclStmt() : Stmt(StmtKind::DeclStmt) {}
};

// ─── Declarations ─────────────────────────────────────────────────────────

enum class DeclKind : uint8_t {
    Module,
    Import,
    Variable,      // let/var
    Function,
    Struct,
    Class,
    Enum,
    EnumCase,
    Protocol,
    Actor,
    Extension,
    Typealias,
    AssociatedType,
    Subscript,
    Init,
    Deinit,
    If,            // if statement
    Guard,
    Switch,
    ForIn,
    While,
    RepeatWhile,
    DoCatch,
    Throw,
    Select,
    Unsafe,
    Attribute,
};

enum class AccessLevel : uint8_t {
    Public,
    Internal,
    FilePrivate,
    Private,
    Open,
};

// Attribute attached to a declaration
struct Attribute {
    std::string name;       // e.g. "main", "cImport", "_cdecl"
    std::vector<std::string> args; // optional arguments
    SourceLocation loc;
};

struct Decl : ASTNode {
    DeclKind declKind;
    AccessLevel access = AccessLevel::Internal;
    bool isStatic = false;
    bool isOverride = false;
    bool isMutating = false;
    bool isAsync = false;
    bool isThrows = false;
    std::vector<Attribute> attributes;
    explicit Decl(DeclKind k) : declKind(k) {}
};

// Module declaration
struct ModuleDecl : Decl {
    std::string name;
    ModuleDecl() : Decl(DeclKind::Module) {}
};

// Import declaration
struct ImportDecl : Decl {
    std::string moduleName;
    ImportDecl() : Decl(DeclKind::Import) {}
};

// Variable declaration (let/var)
struct VariableDecl : Decl {
    bool isLet = true;
    PatternPtr pattern;
    TypeReprPtr typeAnnotation; // optional
    ExprPtr initializer;        // optional
    bool hasWillSet = false;
    bool hasDidSet = false;
    std::vector<StmtPtr> willSetBody;
    std::vector<StmtPtr> didSetBody;
    // Computed property getter/setter bodies
    std::vector<StmtPtr> getterBody;
    std::vector<StmtPtr> setterBody;
    VariableDecl() : Decl(DeclKind::Variable) {}
};

// Function parameter
struct FunctionParam {
    std::string externalLabel; // "_" for unlabeled, "" for no external label
    std::string internalName;
    TypeReprPtr type;
    ExprPtr defaultValue;   // optional
    bool isInOut = false;
    bool isVariadic = false;
    bool isEscaping = false;
    bool isAutoclosure = false;
};

// Function declaration
struct FunctionDecl : Decl {
    std::string name;
    std::vector<std::string> genericParams; // <T, U>
    std::vector<FunctionParam> params;
    TypeReprPtr returnType; // optional, defaults to Void
    bool isAsync = false;
    bool isThrows = false;
    bool isMutating = false;
    bool isOverride = false;
    bool isRequired = false;
    std::vector<StmtPtr> body;
    FunctionDecl() : Decl(DeclKind::Function) {}
};

// Struct/Class/Enum/Protocol/Actor common fields
struct TypeDecl : Decl {
    std::string name;
    std::vector<std::string> genericParams;
    std::vector<TypeReprPtr> conformsTo; // protocols / superclass
    std::vector<DeclPtr> members;
    explicit TypeDecl(DeclKind k) : Decl(k) {}
};

struct StructDecl : TypeDecl {
    StructDecl() : TypeDecl(DeclKind::Struct) {}
};

struct ClassDecl : TypeDecl {
    TypeReprPtr superclass; // : Vehicle
    ClassDecl() : TypeDecl(DeclKind::Class) {}
};

struct EnumCaseDecl : Decl {
    std::string name;
    // Raw value or associated values
    struct AssociatedValue {
        std::string label; // optional
        TypeReprPtr type;
    };
    std::vector<AssociatedValue> associatedValues;
    ExprPtr rawValue; // optional: = 0, = "hello"
    EnumCaseDecl() : Decl(DeclKind::EnumCase) {}
};

struct EnumDecl : TypeDecl {
    TypeReprPtr rawValueType; // : Int, : String
    bool isEnumC = false;     // @enum(C)
    std::vector<std::unique_ptr<EnumCaseDecl>> cases;
    EnumDecl() : TypeDecl(DeclKind::Enum) {}
};

struct ProtocolDecl : TypeDecl {
    ProtocolDecl() : TypeDecl(DeclKind::Protocol) {}
};

struct ActorDecl : TypeDecl {
    ActorDecl() : TypeDecl(DeclKind::Actor) {}
};

struct ExtensionDecl : TypeDecl {
    TypeReprPtr extendedType;
    ExtensionDecl() : TypeDecl(DeclKind::Extension) {}
};

struct TypealiasDecl : Decl {
    std::string name;
    std::vector<std::string> genericParams;
    TypeReprPtr underlyingType;
    TypealiasDecl() : Decl(DeclKind::Typealias) {}
};

struct AssociatedTypeDecl : Decl {
    std::string name;
    TypeReprPtr constraint; // : Hashable
    TypeReprPtr defaultType; // = String
    AssociatedTypeDecl() : Decl(DeclKind::AssociatedType) {}
};

// Subscript declaration
struct SubscriptDecl : Decl {
    std::vector<FunctionParam> params;
    TypeReprPtr returnType;
    std::vector<StmtPtr> getterBody;
    std::vector<StmtPtr> setterBody;
    SubscriptDecl() : Decl(DeclKind::Subscript) {}
};

// Init/deinit
struct InitDecl : Decl {
    std::vector<FunctionParam> params;
    bool isConvenience = false;
    bool isRequired = false;
    std::vector<StmtPtr> body;
    InitDecl() : Decl(DeclKind::Init) {}
};

struct DeinitDecl : Decl {
    std::vector<StmtPtr> body;
    DeinitDecl() : Decl(DeclKind::Deinit) {}
};

// ─── Control Flow Declarations (statements that act like declarations) ────

struct IfDecl : Decl {
    ExprPtr condition;
    std::vector<StmtPtr> thenBody;
    std::vector<StmtPtr> elseBody;
    DeclPtr elseIfDecl; // nested IfDecl for else-if
    IfDecl() : Decl(DeclKind::If) {}
};

struct GuardDecl : Decl {
    ExprPtr condition;
    std::vector<StmtPtr> elseBody;
    GuardDecl() : Decl(DeclKind::Guard) {}
};

struct SwitchCase {
    struct Label {
        // One of: expression, pattern, or wildcard
        ExprPtr expression;    // case 0, case 1..<10
        PatternPtr pattern;    // case let x, case .some(let v)
        bool isDefault = false; // default:
    };
    std::vector<Label> labels; // multiple patterns per case
    std::vector<StmtPtr> body;
    bool isFallthrough = false;
};

struct SwitchDecl : Decl {
    ExprPtr subject;
    std::vector<SwitchCase> cases;
    SwitchDecl() : Decl(DeclKind::Switch) {}
};

struct ForInDecl : Decl {
    PatternPtr pattern;
    ExprPtr sequence; // the thing being iterated
    ExprPtr whereClause; // optional where filter
    std::vector<StmtPtr> body;
    ForInDecl() : Decl(DeclKind::ForIn) {}
};

struct WhileDecl : Decl {
    ExprPtr condition;
    std::vector<StmtPtr> body;
    WhileDecl() : Decl(DeclKind::While) {}
};

struct RepeatWhileDecl : Decl {
    std::vector<StmtPtr> body;
    ExprPtr condition;
    RepeatWhileDecl() : Decl(DeclKind::RepeatWhile) {}
};

struct DoCatchDecl : Decl {
    std::vector<StmtPtr> doBody;
    struct CatchClause {
        PatternPtr pattern; // optional: catch let err as FileError
        ExprPtr whereClause; // optional
        std::vector<StmtPtr> body;
    };
    std::vector<CatchClause> catches;
    DoCatchDecl() : Decl(DeclKind::DoCatch) {}
};

struct ThrowDecl : Decl {
    ExprPtr value;
    ThrowDecl() : Decl(DeclKind::Throw) {}
};

struct SelectCase {
    enum class Kind { Send, Receive, Default };
    Kind kind;
    PatternPtr recvPattern;   // for receive: let x
    ExprPtr sendValue;        // for send: value
    ExprPtr channel;          // the channel expression
    std::vector<StmtPtr> body;
};

struct SelectDecl : Decl {
    std::vector<SelectCase> cases;
    SelectDecl() : Decl(DeclKind::Select) {}
};

struct UnsafeDecl : Decl {
    std::vector<StmtPtr> body;
    UnsafeDecl() : Decl(DeclKind::Unsafe) {}
};

// ─── Compilation Unit (top-level) ─────────────────────────────────────────

struct CompilationUnit : ASTNode {
    std::string filename;
    ModuleDecl* moduleDecl = nullptr; // optional module declaration
    std::vector<DeclPtr> declarations;
};

} // namespace suki
