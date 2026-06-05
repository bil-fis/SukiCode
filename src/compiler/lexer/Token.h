#pragma once
// Token definitions for the SukiCode lexer.
// Each token carries its kind, source location, and optional literal value.

#include <cstdint>
#include <string>
#include <string_view>

namespace suki {

// Source location in a .suki file
struct SourceLocation {
    uint32_t line;
    uint32_t column;
    uint32_t offset; // byte offset from start of file

    SourceLocation() : line(1), column(1), offset(0) {}
    SourceLocation(uint32_t l, uint32_t c, uint32_t o) : line(l), column(c), offset(o) {}
};

// Token kind enumeration
enum class TokenKind : uint16_t {
    // ─── End of file ──────────────────────────────────────────────────────
    Eof,

    // ─── Literals ─────────────────────────────────────────────────────────
    IntegerLiteral,    // 42, 0xFF, 0b1010, 1_000_000
    FloatLiteral,      // 3.14, 1.0e10, 0x1.Ap5
    StringLiteral,     // "hello", "interpolated \(expr)"
    CharLiteral,       // 'A', '\n'
    True,              // true
    False,             // false
    Nil,               // nil

    // ─── Identifiers ──────────────────────────────────────────────────────
    Identifier,        // foo, myVar, _count
    EscapedIdentifier, // `keyword` (backtick-escaped keyword used as identifier)

    // ─── Keywords ─────────────────────────────────────────────────────────
    // Declarations
    KwModule,
    KwImport,
    KwLet,
    KwVar,
    KwFunc,
    KwReturn,
    KwStruct,
    KwClass,
    KwEnum,
    KwProtocol,
    KwExtension,
    KwInit,
    KwDeinit,
    KwSubscript,
    KwOverride,
    KwFinal,
    KwRequired,
    KwConvenience,
    KwTypealias,
    KwAssociatedtype,

    // Control flow
    KwIf,
    KwElse,
    KwGuard,
    KwSwitch,
    KwCase,
    KwDefault,
    KwFor,
    KwIn,
    KwWhile,
    KwRepeat,
    KwBreak,
    KwContinue,
    KwFallthrough,
    KwDefer,
    KwSelect,

    // Error handling
    KwDo,
    KwCatch,
    KwThrows,
    KwThrow,
    KwTry,
    KwAs,
    KwIs,

    // Concurrency
    KwAsync,
    KwAwait,
    KwActor,
    KwNonisolated,

    // Memory / ownership
    KwWeak,
    KwUnowned,
    KwMove,
    KwUnsafe,
    KwAsm,          // asm (inline assembly)

    // Type-related
    KwSelf,
    KwSelfType,     // Self (capital S, type context)
    KwSuper,
    KwSome,         // some Protocol (opaque return type)
    KwAny,          // any Protocol (existential)
    KwWhere,

    // Access control
    KwPublic,
    KwInternal,
    KwFileprivate,
    KwPrivate,
    KwOpen,
    KwStatic,
    KwMutating,
    KwInOut,

    // Properties
    KwGet,
    KwSet,
    KwWillSet,
    KwDidSet,

    // Boolean logic (not used as standalone keywords in expressions,
    // but reserved for future use)
    KwAnd,           // 'and' as alternative to &&
    KwOr,            // 'or' as alternative to ||
    KwNot,           // 'not' as alternative to !

    // ─── Attributes ───────────────────────────────────────────────────────
    AtMain,          // @main
    AtCImport,       // @cImport
    AtCDecl,         // @_cdecl
    AtMacro,         // @macro
    AtEscaping,      // @escaping
    AtAutoclosure,   // @autoclosure
    AtMainActor,     // @MainActor
    AtExecutor,      // @executor
    AtEnumC,         // @enum(C)
    AtNoMangle,      // @no_mangle
    AtPanicHandler,  // @panic_handler
    AtDepends,       // @depends(on:)
    AtFreestanding,  // @freestanding
    AtAttached,      // @attached
    AtTest,          // @test
    AtAttribute,     // @<identifier> (generic attribute)

    // ─── Punctuation ──────────────────────────────────────────────────────
    LParen,          // (
    RParen,          // )
    LBrace,          // {
    RBrace,          // }
    LBracket,        // [
    RBracket,        // ]
    Dot,             // .
    Comma,           // ,
    Colon,           // :
    Semicolon,       // ;
    At,              // @
    Hash,            // #
    Arrow,           // ->
    FatArrow,        // =>
    Underscore,      // _
    Ellipsis,        // ...
    Range,           // ..<
    Backslash,       // \ (used in string interpolation)

    // ─── Operators ────────────────────────────────────────────────────────
    // Assignment
    Assign,          // =
    PlusAssign,      // +=
    MinusAssign,     // -=
    StarAssign,      // *=
    SlashAssign,     // /=
    PercentAssign,   // %=
    AmpAssign,       // &=
    PipeAssign,      // |=
    CaretAssign,     // ^=
    LShiftAssign,    // <<=
    RShiftAssign,    // >>=

    // Arithmetic
    Plus,            // +
    Minus,           // -
    Star,            // *
    Slash,           // /
    Percent,         // %

    // Comparison
    Equal,           // ==
    NotEqual,        // !=
    Less,            // <
    Greater,         // >
    LessEqual,       // <=
    GreaterEqual,    // >=

    // Logical
    AmpAmp,          // &&
    PipePipe,        // ||
    Bang,            // !

    // Bitwise
    Amp,             // &
    Pipe,            // |
    Caret,           // ^
    Tilde,           // ~
    LShift,          // <<
    RShift,          // >>

    // Question mark
    Question,        // ?

    // Null coalescing
    QuestionQuestion, // ??

    // Channel
    LeftArrow,       // <- (used in channel send/receive and select)

    // ─── Error ────────────────────────────────────────────────────────────
    Error,           // Lexer error token
};

// Token literal value variant
enum class LiteralKind : uint8_t {
    None,
    Integer,
    Float,
    String,
    Char,
};

// A single token from the lexer
struct Token {
    TokenKind kind;
    SourceLocation loc;
    uint32_t length;      // byte length of the token text in source

    // Literal value (for numeric/string/char literals)
    LiteralKind literalKind;
    union {
        int64_t intValue;
        double floatValue;
        uint32_t charValue;  // Unicode scalar
    } literal;
    std::string stringValue; // for string literals and identifiers

    Token()
        : kind(TokenKind::Eof), loc(), length(0), literalKind(LiteralKind::None) {
        literal.intValue = 0;
    }

    bool is(TokenKind k) const { return kind == k; }
    bool isNot(TokenKind k) const { return kind != k; }
    bool isOneOf(std::initializer_list<TokenKind> kinds) const;
    bool isKeyword() const;
    bool isLiteral() const;
    bool isOperator() const;
    bool isAssignmentOperator() const;

    // Get the text of this token from the source
    std::string_view text(std::string_view source) const;

    // Human-readable name for the token kind
    static const char* kindName(TokenKind kind);

    // Check if an identifier string is a keyword
    static TokenKind keywordLookup(std::string_view name);

    // Check if a string is an attribute (starts with @)
    static TokenKind attributeLookup(std::string_view name);
};

} // namespace suki
