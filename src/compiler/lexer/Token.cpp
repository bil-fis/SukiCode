// Token implementation for SukiCode lexer.

#include "Token.h"
#include <unordered_map>

namespace suki {

bool Token::isOneOf(std::initializer_list<TokenKind> kinds) const {
    for (auto k : kinds) {
        if (kind == k) return true;
    }
    return false;
}

bool Token::isKeyword() const {
    return kind >= TokenKind::KwModule && kind <= TokenKind::KwNot;
}

bool Token::isLiteral() const {
    return kind >= TokenKind::IntegerLiteral && kind <= TokenKind::Nil;
}

bool Token::isOperator() const {
    return kind >= TokenKind::Assign && kind <= TokenKind::LeftArrow;
}

bool Token::isAssignmentOperator() const {
    return kind >= TokenKind::Assign && kind <= TokenKind::RShiftAssign;
}

std::string_view Token::text(std::string_view source) const {
    return source.substr(loc.offset, length);
}

// ─── Keyword lookup table ─────────────────────────────────────────────────
static const std::unordered_map<std::string_view, TokenKind> keywordTable = {
    // Declarations
    {"module",       TokenKind::KwModule},
    {"import",       TokenKind::KwImport},
    {"let",          TokenKind::KwLet},
    {"var",          TokenKind::KwVar},
    {"func",         TokenKind::KwFunc},
    {"return",       TokenKind::KwReturn},
    {"struct",       TokenKind::KwStruct},
    {"class",        TokenKind::KwClass},
    {"enum",         TokenKind::KwEnum},
    {"protocol",     TokenKind::KwProtocol},
    {"extension",    TokenKind::KwExtension},
    {"init",         TokenKind::KwInit},
    {"deinit",       TokenKind::KwDeinit},
    {"subscript",    TokenKind::KwSubscript},
    {"override",     TokenKind::KwOverride},
    {"final",        TokenKind::KwFinal},
    {"required",     TokenKind::KwRequired},
    {"convenience",  TokenKind::KwConvenience},
    {"typealias",    TokenKind::KwTypealias},
    {"associatedtype", TokenKind::KwAssociatedtype},

    // Control flow
    {"if",           TokenKind::KwIf},
    {"else",         TokenKind::KwElse},
    {"guard",        TokenKind::KwGuard},
    {"switch",       TokenKind::KwSwitch},
    {"case",         TokenKind::KwCase},
    {"default",      TokenKind::KwDefault},
    {"for",          TokenKind::KwFor},
    {"in",           TokenKind::KwIn},
    {"while",        TokenKind::KwWhile},
    {"repeat",       TokenKind::KwRepeat},
    {"break",        TokenKind::KwBreak},
    {"continue",     TokenKind::KwContinue},
    {"fallthrough",  TokenKind::KwFallthrough},
    {"defer",        TokenKind::KwDefer},
    {"select",       TokenKind::KwSelect},

    // Error handling
    {"do",           TokenKind::KwDo},
    {"catch",        TokenKind::KwCatch},
    {"throws",       TokenKind::KwThrows},
    {"throw",        TokenKind::KwThrow},
    {"try",          TokenKind::KwTry},
    {"as",           TokenKind::KwAs},
    {"is",           TokenKind::KwIs},

    // Concurrency
    {"async",        TokenKind::KwAsync},
    {"await",        TokenKind::KwAwait},
    {"actor",        TokenKind::KwActor},
    {"nonisolated",  TokenKind::KwNonisolated},

    // Memory / ownership
    {"weak",         TokenKind::KwWeak},
    {"unowned",      TokenKind::KwUnowned},
    {"move",         TokenKind::KwMove},
    {"unsafe",       TokenKind::KwUnsafe},
    {"extern",       TokenKind::KwExtern},
    {"asm",          TokenKind::KwAsm},

    // Type-related
    {"self",         TokenKind::KwSelf},
    {"Self",         TokenKind::KwSelfType},
    {"super",        TokenKind::KwSuper},
    {"some",         TokenKind::KwSome},
    {"any",          TokenKind::KwAny},
    {"where",        TokenKind::KwWhere},

    // Access control
    {"public",       TokenKind::KwPublic},
    {"internal",     TokenKind::KwInternal},
    {"fileprivate",  TokenKind::KwFileprivate},
    {"private",      TokenKind::KwPrivate},
    {"open",         TokenKind::KwOpen},
    {"static",       TokenKind::KwStatic},
    {"mutating",     TokenKind::KwMutating},
    {"inout",        TokenKind::KwInOut},

    // Properties
    {"get",          TokenKind::KwGet},
    {"set",          TokenKind::KwSet},
    {"willSet",      TokenKind::KwWillSet},
    {"didSet",       TokenKind::KwDidSet},

    // Boolean alternatives
    {"and",          TokenKind::KwAnd},
    {"or",           TokenKind::KwOr},
    {"not",          TokenKind::KwNot},

    // Literals
    {"true",         TokenKind::True},
    {"false",        TokenKind::False},
    {"nil",          TokenKind::Nil},
};

TokenKind Token::keywordLookup(std::string_view name) {
    auto it = keywordTable.find(name);
    if (it != keywordTable.end()) return it->second;
    return TokenKind::Identifier;
}

TokenKind Token::attributeLookup(std::string_view name) {
    // name should include the '@' prefix
    if (name == "@main")          return TokenKind::AtMain;
    if (name == "@cImport")       return TokenKind::AtCImport;
    if (name == "@_cdecl")        return TokenKind::AtCDecl;
    if (name == "@macro")         return TokenKind::AtMacro;
    if (name == "@escaping")      return TokenKind::AtEscaping;
    if (name == "@autoclosure")   return TokenKind::AtAutoclosure;
    if (name == "@MainActor")     return TokenKind::AtMainActor;
    if (name == "@executor")      return TokenKind::AtExecutor;
    if (name == "@enum(C)")       return TokenKind::AtEnumC;
    if (name == "@no_mangle")     return TokenKind::AtNoMangle;
    if (name == "@panic_handler") return TokenKind::AtPanicHandler;
    if (name == "@depends")       return TokenKind::AtDepends;
    if (name == "@freestanding")  return TokenKind::AtFreestanding;
    if (name == "@attached")      return TokenKind::AtAttached;
    if (name == "@test")          return TokenKind::AtTest;
    return TokenKind::AtAttribute;
}

const char* Token::kindName(TokenKind kind) {
    switch (kind) {
        case TokenKind::Eof:              return "end of file";
        case TokenKind::IntegerLiteral:   return "integer literal";
        case TokenKind::FloatLiteral:     return "float literal";
        case TokenKind::StringLiteral:    return "string literal";
        case TokenKind::CharLiteral:      return "character literal";
        case TokenKind::True:             return "'true'";
        case TokenKind::False:            return "'false'";
        case TokenKind::Nil:              return "'nil'";
        case TokenKind::Identifier:       return "identifier";
        case TokenKind::EscapedIdentifier: return "escaped identifier";

        // Keywords
        case TokenKind::KwModule:    return "'module'";
        case TokenKind::KwImport:    return "'import'";
        case TokenKind::KwLet:       return "'let'";
        case TokenKind::KwVar:       return "'var'";
        case TokenKind::KwFunc:      return "'func'";
        case TokenKind::KwReturn:    return "'return'";
        case TokenKind::KwStruct:    return "'struct'";
        case TokenKind::KwClass:     return "'class'";
        case TokenKind::KwEnum:      return "'enum'";
        case TokenKind::KwProtocol:  return "'protocol'";
        case TokenKind::KwExtension: return "'extension'";
        case TokenKind::KwInit:      return "'init'";
        case TokenKind::KwDeinit:    return "'deinit'";
        case TokenKind::KwSubscript: return "'subscript'";
        case TokenKind::KwOverride:  return "'override'";
        case TokenKind::KwFinal:     return "'final'";
        case TokenKind::KwRequired:  return "'required'";
        case TokenKind::KwConvenience: return "'convenience'";
        case TokenKind::KwTypealias: return "'typealias'";
        case TokenKind::KwAssociatedtype: return "'associatedtype'";
        case TokenKind::KwIf:        return "'if'";
        case TokenKind::KwElse:      return "'else'";
        case TokenKind::KwGuard:     return "'guard'";
        case TokenKind::KwSwitch:    return "'switch'";
        case TokenKind::KwCase:      return "'case'";
        case TokenKind::KwDefault:   return "'default'";
        case TokenKind::KwFor:       return "'for'";
        case TokenKind::KwIn:        return "'in'";
        case TokenKind::KwWhile:     return "'while'";
        case TokenKind::KwRepeat:    return "'repeat'";
        case TokenKind::KwBreak:     return "'break'";
        case TokenKind::KwContinue:  return "'continue'";
        case TokenKind::KwFallthrough: return "'fallthrough'";
        case TokenKind::KwDefer:     return "'defer'";
        case TokenKind::KwSelect:    return "'select'";
        case TokenKind::KwDo:        return "'do'";
        case TokenKind::KwCatch:     return "'catch'";
        case TokenKind::KwThrows:    return "'throws'";
        case TokenKind::KwThrow:     return "'throw'";
        case TokenKind::KwTry:       return "'try'";
        case TokenKind::KwAs:        return "'as'";
        case TokenKind::KwIs:        return "'is'";
        case TokenKind::KwAsync:     return "'async'";
        case TokenKind::KwAwait:     return "'await'";
        case TokenKind::KwActor:     return "'actor'";
        case TokenKind::KwNonisolated: return "'nonisolated'";
        case TokenKind::KwWeak:      return "'weak'";
        case TokenKind::KwUnowned:   return "'unowned'";
        case TokenKind::KwMove:      return "'move'";
        case TokenKind::KwUnsafe:    return "'unsafe'";
        case TokenKind::KwAsm:       return "'asm'";
        case TokenKind::KwSelf:      return "'self'";
        case TokenKind::KwSelfType:  return "'Self'";
        case TokenKind::KwSuper:     return "'super'";
        case TokenKind::KwSome:      return "'some'";
        case TokenKind::KwAny:       return "'any'";
        case TokenKind::KwWhere:     return "'where'";
        case TokenKind::KwPublic:    return "'public'";
        case TokenKind::KwInternal:  return "'internal'";
        case TokenKind::KwFileprivate: return "'fileprivate'";
        case TokenKind::KwPrivate:   return "'private'";
        case TokenKind::KwOpen:      return "'open'";
        case TokenKind::KwStatic:    return "'static'";
        case TokenKind::KwMutating:  return "'mutating'";
        case TokenKind::KwInOut:     return "'inout'";
        case TokenKind::KwGet:       return "'get'";
        case TokenKind::KwSet:       return "'set'";
        case TokenKind::KwWillSet:   return "'willSet'";
        case TokenKind::KwDidSet:    return "'didSet'";
        case TokenKind::KwAnd:       return "'and'";
        case TokenKind::KwOr:        return "'or'";
        case TokenKind::KwNot:       return "'not'";

        // Attributes
        case TokenKind::AtMain:          return "@main";
        case TokenKind::AtCImport:       return "@cImport";
        case TokenKind::AtCDecl:         return "@_cdecl";
        case TokenKind::AtMacro:         return "@macro";
        case TokenKind::AtEscaping:      return "@escaping";
        case TokenKind::AtAutoclosure:   return "@autoclosure";
        case TokenKind::AtMainActor:     return "@MainActor";
        case TokenKind::AtExecutor:      return "@executor";
        case TokenKind::AtEnumC:         return "@enum(C)";
        case TokenKind::AtNoMangle:      return "@no_mangle";
        case TokenKind::AtPanicHandler:  return "@panic_handler";
        case TokenKind::AtDepends:       return "@depends";
        case TokenKind::AtFreestanding:  return "@freestanding";
        case TokenKind::AtAttached:      return "@attached";
        case TokenKind::AtTest:          return "@test";
        case TokenKind::AtAttribute:     return "attribute";

        // Punctuation
        case TokenKind::LParen:      return "'('";
        case TokenKind::RParen:      return "')'";
        case TokenKind::LBrace:      return "'{'";
        case TokenKind::RBrace:      return "'}'";
        case TokenKind::LBracket:    return "'['";
        case TokenKind::RBracket:    return "']'";
        case TokenKind::Dot:         return "'.'";
        case TokenKind::Comma:       return "','";
        case TokenKind::Colon:       return "':'";
        case TokenKind::Semicolon:   return "';'";
        case TokenKind::At:          return "'@'";
        case TokenKind::Hash:        return "'#'";
        case TokenKind::Arrow:       return "'->'";
        case TokenKind::FatArrow:    return "'=>'";
        case TokenKind::Underscore:  return "'_'";
        case TokenKind::Ellipsis:    return "'...'";
        case TokenKind::Range:       return "'..<'";
        case TokenKind::Backslash:   return "'\\'";

        // Operators
        case TokenKind::Assign:       return "'='";
        case TokenKind::PlusAssign:   return "'+='";
        case TokenKind::MinusAssign:  return "'-='";
        case TokenKind::StarAssign:   return "'*='";
        case TokenKind::SlashAssign:  return "'/='";
        case TokenKind::PercentAssign: return "'%='";
        case TokenKind::AmpAssign:    return "'&='";
        case TokenKind::PipeAssign:   return "'|='";
        case TokenKind::CaretAssign:  return "'^='";
        case TokenKind::LShiftAssign: return "'<<='";
        case TokenKind::RShiftAssign: return "'>>='";
        case TokenKind::Plus:         return "'+'";
        case TokenKind::Minus:        return "'-'";
        case TokenKind::Star:         return "'*'";
        case TokenKind::Slash:        return "'/'";
        case TokenKind::Percent:      return "'%'";
        case TokenKind::Equal:        return "'=='";
        case TokenKind::NotEqual:     return "'!='";
        case TokenKind::Less:         return "'<'";
        case TokenKind::Greater:      return "'>'";
        case TokenKind::LessEqual:    return "'<='";
        case TokenKind::GreaterEqual: return "'>='";
        case TokenKind::AmpAmp:       return "'&&'";
        case TokenKind::PipePipe:     return "'||'";
        case TokenKind::Bang:         return "'!'";
        case TokenKind::Amp:          return "'&'";
        case TokenKind::Pipe:         return "'|'";
        case TokenKind::Caret:        return "'^'";
        case TokenKind::Tilde:        return "'~'";
        case TokenKind::LShift:       return "'<<'";
        case TokenKind::RShift:       return "'>>'";
        case TokenKind::Question:     return "'?'";
        case TokenKind::QuestionQuestion: return "'??'";
        case TokenKind::LeftArrow:    return "'<-'";
        case TokenKind::Error:        return "error";
    }
    return "unknown";
}

} // namespace suki
