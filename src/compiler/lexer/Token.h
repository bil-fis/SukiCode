#pragma once

#include "compiler/diag/Diagnostic.h"
#include <string>
#include <vector>

namespace suki {

// ─── Token kinds ──────────────────────────────────────────────────────────
enum class TokenKind {
    TK_EOF,
    TK_Error,
    TK_Identifier,
    TK_IntLiteral,
    TK_FloatLiteral,
    TK_StringLiteral,   // whole string without interpolation
    TK_StringFragment,  // text fragment of an interpolated string
    TK_StringEnd,       // marks the end of an interpolated string
    TK_CharLiteral,
    TK_Keyword,
    TK_Punctuator,
};

// ─── Keywords (subset of Swift-like SukiCode surface syntax) ────────────────
enum class KeywordID {
    None,
    // declarations
    Module, Import, Let, Var, Func, Struct, Enum, Class, Protocol, Extension,
    Typealias, Associatedtype, Init, Deinit, Subscript,
    // control flow
    If, Else, Guard, Switch, Case, Default, For, In, While, Repeat,
    Break, Continue, Fallthrough, Return,
    // patterns / literals / self
    Where, As, Is, Nil, True, False, Self, Super,
    // error handling
    Throw, Throws, Rethrows, Try, Catch, Do,
    // access & modifiers
    Public, Private, Internal, Fileprivate, Open, Static, Final, Override,
    Required, Convenience, Lazy, Inout, Mutating, Nonmutating,
    Get, Set, WillSet, DidSet,
    // memory model
    Weak, Unowned, Owned, Unsafe, Move,
    // concurrency
    Async, Await, Actor, Channel, Select, Spawn, Task,
    // generics / meta programming
    Some, Macro,
    // attributes / ffi
    Foreign, Extern, Defer,
    // NOTE: memory-model directives (memory/model/gc/arc/pool) are intentionally
    // NOT reserved as keywords here; they are regular identifiers so that names
    // like `pool` / `Arc` / `MemoryPool` parse as identifiers. They may become
    // contextual keywords when that syntax is implemented in Sema.
};

// ─── Punctuators / operators ───────────────────────────────────────────────
enum class PunctuatorID {
    None,
    Plus, Minus, Star, Slash, Percent,
    PlusEqual, MinusEqual, StarEqual, SlashEqual, PercentEqual,
    Equal, EqualEqual, BangEqual, Less, Greater, LessEqual, GreaterEqual,
    LeftArrow,                  // <-  channel send / receive
    AmpAmp, PipePipe, Bang, Amp, Pipe, Caret, Tilde,
    LessLess, GreaterGreater,
    DotDotLess, DotDot,        // ..<   ...
    QuestionQuestion,          // ??
    Question, Colon, Semicolon, Comma, Dot,
    LParen, RParen, LBrace, RBrace, LBracket, RBracket,
    At, Hash, Arrow, FatArrow, // ->  =>
};

// ─── Token ──────────────────────────────────────────────────────────────────
struct Token {
    TokenKind kind = TokenKind::TK_EOF;
    SourceLocation loc;
    std::string text;            // raw source text

    // literal payload
    std::string stringValue;     // decoded text (StringLiteral/Fragment/Char)
    std::string numberText;      // original numeric text, including suffix

    // keyword / punctuator discriminant
    KeywordID keyword = KeywordID::None;
    PunctuatorID punct = PunctuatorID::None;

    // doc comment accumulated from preceding /// or /** */ lines
    std::string docComment;

    bool isKeyword(KeywordID k) const { return kind == TokenKind::TK_Keyword && keyword == k; }
    bool isPunct(PunctuatorID p) const { return kind == TokenKind::TK_Punctuator && punct == p; }
};

// ─── Keyword mapping ─────────────────────────────────────────────────────────
inline KeywordID keywordFromString(const std::string& s) {
    static const std::vector<std::pair<std::string, KeywordID>> table = {
        {"module", KeywordID::Module}, {"import", KeywordID::Import},
        {"let", KeywordID::Let}, {"var", KeywordID::Var}, {"func", KeywordID::Func},
        {"struct", KeywordID::Struct}, {"enum", KeywordID::Enum}, {"class", KeywordID::Class},
        {"protocol", KeywordID::Protocol}, {"extension", KeywordID::Extension},
        {"typealias", KeywordID::Typealias}, {"associatedtype", KeywordID::Associatedtype},
        {"init", KeywordID::Init}, {"deinit", KeywordID::Deinit}, {"subscript", KeywordID::Subscript},
        {"if", KeywordID::If}, {"else", KeywordID::Else}, {"guard", KeywordID::Guard},
        {"switch", KeywordID::Switch}, {"case", KeywordID::Case}, {"default", KeywordID::Default},
        {"for", KeywordID::For}, {"in", KeywordID::In}, {"while", KeywordID::While},
        {"repeat", KeywordID::Repeat}, {"break", KeywordID::Break}, {"continue", KeywordID::Continue},
        {"fallthrough", KeywordID::Fallthrough}, {"return", KeywordID::Return},
        {"where", KeywordID::Where}, {"as", KeywordID::As}, {"is", KeywordID::Is},
        {"nil", KeywordID::Nil}, {"true", KeywordID::True}, {"false", KeywordID::False},
        {"self", KeywordID::Self}, {"Self", KeywordID::Self}, {"super", KeywordID::Super},
        {"throw", KeywordID::Throw}, {"throws", KeywordID::Throws}, {"rethrows", KeywordID::Rethrows},
        {"try", KeywordID::Try}, {"catch", KeywordID::Catch}, {"do", KeywordID::Do},
        {"public", KeywordID::Public}, {"private", KeywordID::Private},
        {"internal", KeywordID::Internal}, {"fileprivate", KeywordID::Fileprivate},
        // NOTE: `open` is deliberately *not* reserved. The spec (§10.1) defines
        // only four access levels (public / internal / fileprivate / private),
        // so reserving it would make a perfectly ordinary identifier such as
        // `var open = 0` fail to parse.
        {"static", KeywordID::Static}, {"final", KeywordID::Final},
        {"override", KeywordID::Override}, {"required", KeywordID::Required},
        {"convenience", KeywordID::Convenience}, {"lazy", KeywordID::Lazy},
        {"inout", KeywordID::Inout}, {"mutating", KeywordID::Mutating},
        {"nonmutating", KeywordID::Nonmutating}, {"get", KeywordID::Get},
        {"set", KeywordID::Set}, {"willSet", KeywordID::WillSet}, {"didSet", KeywordID::DidSet},
        {"weak", KeywordID::Weak},
        {"unsafe", KeywordID::Unsafe}, {"move", KeywordID::Move},
        {"async", KeywordID::Async}, {"await", KeywordID::Await}, {"actor", KeywordID::Actor},
        {"select", KeywordID::Select},
        // NOTE: `owned` / `unowned` / `channel` / `spawn` / `task` are intentionally
        // NOT reserved; they are common identifiers and may be used as names.
        // Type-level ownership annotations are recognised contextually in Sema.
        {"some", KeywordID::Some}, {"macro", KeywordID::Macro},
        {"foreign", KeywordID::Foreign}, {"extern", KeywordID::Extern}, {"defer", KeywordID::Defer},
    };
    for (const auto& kv : table) {
        if (kv.first == s) return kv.second;
    }
    return KeywordID::None;
}

inline const char* keywordToString(KeywordID k) {
    switch (k) {
        case KeywordID::Module: return "module";
        case KeywordID::Import: return "import";
        case KeywordID::Let: return "let";
        case KeywordID::Var: return "var";
        case KeywordID::Func: return "func";
        case KeywordID::Struct: return "struct";
        case KeywordID::Enum: return "enum";
        case KeywordID::Class: return "class";
        case KeywordID::Protocol: return "protocol";
        case KeywordID::Extension: return "extension";
        case KeywordID::Typealias: return "typealias";
        case KeywordID::Associatedtype: return "associatedtype";
        case KeywordID::Init: return "init";
        case KeywordID::Deinit: return "deinit";
        case KeywordID::Subscript: return "subscript";
        case KeywordID::If: return "if";
        case KeywordID::Else: return "else";
        case KeywordID::Guard: return "guard";
        case KeywordID::Switch: return "switch";
        case KeywordID::Case: return "case";
        case KeywordID::Default: return "default";
        case KeywordID::For: return "for";
        case KeywordID::In: return "in";
        case KeywordID::While: return "while";
        case KeywordID::Repeat: return "repeat";
        case KeywordID::Break: return "break";
        case KeywordID::Continue: return "continue";
        case KeywordID::Fallthrough: return "fallthrough";
        case KeywordID::Return: return "return";
        case KeywordID::Where: return "where";
        case KeywordID::As: return "as";
        case KeywordID::Is: return "is";
        case KeywordID::Nil: return "nil";
        case KeywordID::True: return "true";
        case KeywordID::False: return "false";
        case KeywordID::Self: return "self";
        case KeywordID::Super: return "super";
        case KeywordID::Throw: return "throw";
        case KeywordID::Throws: return "throws";
        case KeywordID::Rethrows: return "rethrows";
        case KeywordID::Try: return "try";
        case KeywordID::Catch: return "catch";
        case KeywordID::Do: return "do";
        case KeywordID::Public: return "public";
        case KeywordID::Private: return "private";
        case KeywordID::Internal: return "internal";
        case KeywordID::Fileprivate: return "fileprivate";
        case KeywordID::Open: return "open";
        case KeywordID::Static: return "static";
        case KeywordID::Final: return "final";
        case KeywordID::Override: return "override";
        case KeywordID::Required: return "required";
        case KeywordID::Convenience: return "convenience";
        case KeywordID::Lazy: return "lazy";
        case KeywordID::Inout: return "inout";
        case KeywordID::Mutating: return "mutating";
        case KeywordID::Nonmutating: return "nonmutating";
        case KeywordID::Get: return "get";
        case KeywordID::Set: return "set";
        case KeywordID::WillSet: return "willSet";
        case KeywordID::DidSet: return "didSet";
        case KeywordID::Weak: return "weak";
        case KeywordID::Unowned: return "unowned";
        case KeywordID::Owned: return "owned";
        case KeywordID::Unsafe: return "unsafe";
        case KeywordID::Move: return "move";
        case KeywordID::Async: return "async";
        case KeywordID::Await: return "await";
        case KeywordID::Actor: return "actor";
        case KeywordID::Channel: return "channel";
        case KeywordID::Select: return "select";
        case KeywordID::Spawn: return "spawn";
        case KeywordID::Task: return "task";
        case KeywordID::Some: return "some";
        case KeywordID::Macro: return "macro";
        case KeywordID::Foreign: return "foreign";
        case KeywordID::Extern: return "extern";
        case KeywordID::Defer: return "defer";
        default: return "<unknown-keyword>";
    }
}

inline const char* punctToString(PunctuatorID p) {
    switch (p) {
        case PunctuatorID::Plus: return "+";
        case PunctuatorID::Minus: return "-";
        case PunctuatorID::Star: return "*";
        case PunctuatorID::Slash: return "/";
        case PunctuatorID::Percent: return "%";
        case PunctuatorID::PlusEqual: return "+=";
        case PunctuatorID::MinusEqual: return "-=";
        case PunctuatorID::StarEqual: return "*=";
        case PunctuatorID::SlashEqual: return "/=";
        case PunctuatorID::PercentEqual: return "%=";
        case PunctuatorID::Equal: return "=";
        case PunctuatorID::EqualEqual: return "==";
        case PunctuatorID::BangEqual: return "!=";
        case PunctuatorID::Less: return "<";
        case PunctuatorID::Greater: return ">";
        case PunctuatorID::LessEqual: return "<=";
        case PunctuatorID::GreaterEqual: return ">=";
        case PunctuatorID::LeftArrow: return "<-";
        case PunctuatorID::AmpAmp: return "&&";
        case PunctuatorID::PipePipe: return "||";
        case PunctuatorID::Bang: return "!";
        case PunctuatorID::Amp: return "&";
        case PunctuatorID::Pipe: return "|";
        case PunctuatorID::Caret: return "^";
        case PunctuatorID::Tilde: return "~";
        case PunctuatorID::LessLess: return "<<";
        case PunctuatorID::GreaterGreater: return ">>";
        case PunctuatorID::DotDotLess: return "..<";
        case PunctuatorID::DotDot: return "...";
        case PunctuatorID::QuestionQuestion: return "??";
        case PunctuatorID::Question: return "?";
        case PunctuatorID::Colon: return ":";
        case PunctuatorID::Semicolon: return ";";
        case PunctuatorID::Comma: return ",";
        case PunctuatorID::Dot: return ".";
        case PunctuatorID::LParen: return "(";
        case PunctuatorID::RParen: return ")";
        case PunctuatorID::LBrace: return "{";
        case PunctuatorID::RBrace: return "}";
        case PunctuatorID::LBracket: return "[";
        case PunctuatorID::RBracket: return "]";
        case PunctuatorID::At: return "@";
        case PunctuatorID::Hash: return "#";
        case PunctuatorID::Arrow: return "->";
        case PunctuatorID::FatArrow: return "=>";
        default: return "<unknown-punct>";
    }
}

} // namespace suki
