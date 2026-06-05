// SukiCode Lexer implementation.
// Scans UTF-8 source code into tokens.

#include "Lexer.h"
#include <cctype>
#include <cstdlib>
#include <charconv>
#include <algorithm>

namespace suki {

Lexer::Lexer(std::string_view source, std::string_view filename, DiagnosticEngine& diag)
    : source_(source), filename_(filename), pos_(0), line_(1), lineStart_(0), diag_(diag) {}

Lexer::~Lexer() = default;

std::vector<Token> Lexer::lexAll() {
    std::vector<Token> tokens;
    while (true) {
        Token tok = next();
        tokens.push_back(tok);
        if (tok.is(TokenKind::Eof)) break;
    }
    return tokens;
}

Token Lexer::next() {
    skipWhitespace();
    if (pos_ >= source_.size()) {
        Token tok;
        tok.kind = TokenKind::Eof;
        tok.loc = {line_, pos_ - lineStart_ + 1, pos_};
        tok.length = 0;
        return tok;
    }
    return scanToken();
}

// ─── Source navigation ─────────────────────────────────────────────────────

char Lexer::peek() const {
    if (pos_ >= source_.size()) return 0;
    return source_[pos_];
}

char Lexer::peekAt(uint32_t offset) const {
    uint32_t p = pos_ + offset;
    if (p >= source_.size()) return 0;
    return source_[p];
}

char Lexer::advance() {
    char c = peek();
    if (c == '\n') {
        line_++;
        lineStart_ = pos_ + 1;
    }
    pos_++;
    return c;
}

void Lexer::skipWhitespace() {
    while (pos_ < source_.size()) {
        char c = peek();
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance();
        } else if (c == '/' && peekAt(1) == '/') {
            scanLineComment();
        } else if (c == '/' && peekAt(1) == '*') {
            scanBlockComment();
        } else {
            break;
        }
    }
}

bool Lexer::match(char expected) {
    if (pos_ < source_.size() && source_[pos_] == expected) {
        advance();
        return true;
    }
    return false;
}

// ─── Character classification ──────────────────────────────────────────────

bool Lexer::isHexDigit(char c) const {
    return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool Lexer::isAlpha(char c) const {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool Lexer::isAlphaNumeric(char c) const {
    return isAlpha(c) || isDigit(c);
}

bool Lexer::isIdentStart(char c) const {
    // Allow Unicode letters and underscore as identifier start
    return isAlpha(c) || c == '_';
}

bool Lexer::isIdentContinue(char c) const {
    return isAlphaNumeric(c) || c == '_';
}

// ─── Token creation ───────────────────────────────────────────────────────

Token Lexer::makeToken(TokenKind kind, uint32_t startPos) {
    Token tok;
    tok.kind = kind;
    tok.loc = {line_, startPos - lineStart_ + 1, startPos};
    tok.length = pos_ - startPos;
    tok.literalKind = LiteralKind::None;
    tok.literal.intValue = 0;
    return tok;
}

Token Lexer::makeErrorToken(std::string_view message, uint32_t startPos) {
    diag_.error({line_, startPos - lineStart_ + 1, startPos}, filename_, message);
    return makeToken(TokenKind::Error, startPos);
}

// ─── Main scan dispatch ───────────────────────────────────────────────────

Token Lexer::scanToken() {
    uint32_t startPos = pos_;
    char c = advance();

    switch (c) {
        // Single-character tokens
        case '(': return makeToken(TokenKind::LParen, startPos);
        case ')': return makeToken(TokenKind::RParen, startPos);
        case '{': return makeToken(TokenKind::LBrace, startPos);
        case '}': return makeToken(TokenKind::RBrace, startPos);
        case '[': return makeToken(TokenKind::LBracket, startPos);
        case ']': return makeToken(TokenKind::RBracket, startPos);
        case ',': return makeToken(TokenKind::Comma, startPos);
        case ';': return makeToken(TokenKind::Semicolon, startPos);
        case '#': return makeToken(TokenKind::Hash, startPos);
        case '~': return makeToken(TokenKind::Tilde, startPos);

        case '.':
            if (peek() == '.' && peekAt(1) == '.') {
                advance(); advance();
                return makeToken(TokenKind::Ellipsis, startPos);
            }
            if (peek() == '.' && peekAt(1) == '<') {
                advance(); advance();
                return makeToken(TokenKind::Range, startPos);
            }
            return makeToken(TokenKind::Dot, startPos);

        case ':': return makeToken(TokenKind::Colon, startPos);

        case '@': return scanAttribute();

        case '_':
            // Check if this is just an underscore token or start of identifier
            if (isIdentContinue(peek())) {
                pos_--; // un-advance to rescan the underscore
                return scanIdentifier(); // _foo is an identifier
            }
            return makeToken(TokenKind::Underscore, startPos);

        // Arrow operators
        case '-':
            if (match('>')) return makeToken(TokenKind::Arrow, startPos);
            if (match('=')) return makeToken(TokenKind::MinusAssign, startPos);
            return makeToken(TokenKind::Minus, startPos);

        case '=':
            if (match('=')) return makeToken(TokenKind::Equal, startPos);
            if (match('>')) return makeToken(TokenKind::FatArrow, startPos);
            return makeToken(TokenKind::Assign, startPos);

        case '!':
            if (match('=')) return makeToken(TokenKind::NotEqual, startPos);
            return makeToken(TokenKind::Bang, startPos);

        case '+':
            if (match('=')) return makeToken(TokenKind::PlusAssign, startPos);
            return makeToken(TokenKind::Plus, startPos);

        case '*':
            if (match('=')) return makeToken(TokenKind::StarAssign, startPos);
            return makeToken(TokenKind::Star, startPos);

        case '%':
            if (match('=')) return makeToken(TokenKind::PercentAssign, startPos);
            return makeToken(TokenKind::Percent, startPos);

        case '&':
            if (match('&')) return makeToken(TokenKind::AmpAmp, startPos);
            if (match('=')) return makeToken(TokenKind::AmpAssign, startPos);
            return makeToken(TokenKind::Amp, startPos);

        case '|':
            if (match('|')) return makeToken(TokenKind::PipePipe, startPos);
            if (match('=')) return makeToken(TokenKind::PipeAssign, startPos);
            return makeToken(TokenKind::Pipe, startPos);

        case '^':
            if (match('=')) return makeToken(TokenKind::CaretAssign, startPos);
            return makeToken(TokenKind::Caret, startPos);

        case '<':
            if (match('<')) {
                if (match('=')) return makeToken(TokenKind::LShiftAssign, startPos);
                return makeToken(TokenKind::LShift, startPos);
            }
            if (match('=')) return makeToken(TokenKind::LessEqual, startPos);
            // '<-' is the channel send/receive operator
            if (match('-')) return makeToken(TokenKind::LeftArrow, startPos);
            return makeToken(TokenKind::Less, startPos);

        case '>':
            if (match('>')) {
                if (match('=')) return makeToken(TokenKind::RShiftAssign, startPos);
                return makeToken(TokenKind::RShift, startPos);
            }
            if (match('=')) return makeToken(TokenKind::GreaterEqual, startPos);
            return makeToken(TokenKind::Greater, startPos);

        case '?':
            if (match('?')) return makeToken(TokenKind::QuestionQuestion, startPos);
            return makeToken(TokenKind::Question, startPos);

        case '/':
            if (match('=')) return makeToken(TokenKind::SlashAssign, startPos);
            return makeToken(TokenKind::Slash, startPos);

        case '\\':
            // Backslash is used for string interpolation \(...)
            // Outside strings, it's an error
            return makeToken(TokenKind::Backslash, startPos);

        // Literals
        case '"':
            // Check for raw string: r"..."
            // (raw strings start with 'r', handled in scanIdentifier)
            if (peek() == '"' && peekAt(1) == '"') {
                // Start of multiline string """
                advance(); advance();
                return scanMultilineString();
            }
            return scanString();

        case '\'':
            return scanChar();

        default:
            // Numbers
            if (isDigit(c)) {
                pos_--; // un-advance to rescan the digit
                return scanNumber();
            }
            // Identifiers and keywords
            if (isIdentStart(c) || (unsigned char)c > 0x7F) {
                pos_--; // un-advance to rescan
                return scanIdentifier();
            }
            return makeErrorToken("unexpected character", startPos);
    }
}

// ─── Number scanning ──────────────────────────────────────────────────────

Token Lexer::scanNumber() {
    uint32_t startPos = pos_;
    bool isFloat = false;
    int base = 10;

    // Check for base prefix: 0x, 0b, 0o
    if (peek() == '0') {
        char next = peekAt(1);
        if (next == 'x' || next == 'X') {
            base = 16;
            advance(); advance(); // skip 0x
        } else if (next == 'b' || next == 'B') {
            base = 2;
            advance(); advance(); // skip 0b
        } else if (next == 'o' || next == 'O') {
            base = 8;
            advance(); advance(); // skip 0o
        }
    }

    // Scan digits
    while (pos_ < source_.size()) {
        char c = peek();
        if (c == '_') {
            advance(); // skip underscores in numbers: 1_000_000
            continue;
        }
        if (base == 16) {
            if (!isHexDigit(c)) break;
        } else if (base == 2) {
            if (c != '0' && c != '1') break;
        } else if (base == 8) {
            if (c < '0' || c > '7') break;
        } else {
            if (!isDigit(c)) break;
        }
        advance();
    }

    // Check for decimal point (only for base 10 and 16)
    if (base == 10 && peek() == '.' && isDigit(peekAt(1))) {
        isFloat = true;
        advance(); // skip '.'
        while (pos_ < source_.size() && (isDigit(peek()) || peek() == '_')) {
            advance();
        }
    }

    // Check for exponent: e+, e-, E+, E-, e, E
    if ((base == 10 || base == 16) && (peek() == 'e' || peek() == 'E' ||
        (base == 16 && (peek() == 'p' || peek() == 'P')))) {
        isFloat = true;
        advance();
        if (peek() == '+' || peek() == '-') advance();
        while (pos_ < source_.size() && (isDigit(peek()) || peek() == '_')) {
            advance();
        }
    }

    // Type suffixes: f (Float), d (Double), u (UInt)
    if (peek() == 'f' || peek() == 'F') {
        isFloat = true;
        advance();
    } else if (peek() == 'd' || peek() == 'D') {
        isFloat = true;
        advance();
    } else if (peek() == 'u' || peek() == 'U') {
        advance(); // unsigned suffix
    }

    Token tok = makeToken(isFloat ? TokenKind::FloatLiteral : TokenKind::IntegerLiteral, startPos);

    // Parse the literal value
    std::string_view text = tok.text(source_);
    std::string cleanText(text);
    cleanText.erase(std::remove(cleanText.begin(), cleanText.end(), '_'), cleanText.end());

    if (isFloat) {
        tok.literalKind = LiteralKind::Float;
        tok.literal.floatValue = std::strtod(cleanText.c_str(), nullptr);
    } else {
        tok.literalKind = LiteralKind::Integer;
        // MSVC's strtoll may not support 0b/0o prefixes; strip them
        const char* numStart = cleanText.c_str();
        if (base == 2 && cleanText.size() > 2 && cleanText[0] == '0' &&
            (cleanText[1] == 'b' || cleanText[1] == 'B')) {
            numStart += 2; // skip "0b"
        } else if (base == 8 && cleanText.size() > 2 && cleanText[0] == '0' &&
                   (cleanText[1] == 'o' || cleanText[1] == 'O')) {
            numStart += 2; // skip "0o"
        }
        tok.literal.intValue = std::strtoll(numStart, nullptr, base);
    }

    return tok;
}

// ─── String scanning ──────────────────────────────────────────────────────

Token Lexer::scanString() {
    uint32_t startPos = pos_ - 1; // include opening "
    std::string value;

    while (pos_ < source_.size()) {
        char c = peek();

        if (c == '"') {
            advance(); // closing "
            Token tok = makeToken(TokenKind::StringLiteral, startPos);
            tok.literalKind = LiteralKind::String;
            tok.stringValue = std::move(value);
            return tok;
        }

        if (c == '\\') {
            advance(); // skip backslash
            if (pos_ >= source_.size()) break;
            char esc = advance();
            switch (esc) {
                case 'n':  value += '\n'; break;
                case 't':  value += '\t'; break;
                case 'r':  value += '\r'; break;
                case '\\': value += '\\'; break;
                case '"':  value += '"';  break;
                case '\'': value += '\''; break;
                case '0':  value += '\0'; break;
                case '(': {
                    // String interpolation \(expr)
                    // For now, we'll treat the whole string up to here as a literal,
                    // and let the parser handle interpolation.
                    // Mark this as a string with interpolation.
                    // TODO: implement proper interpolation tokenization
                    // For now, just include the literal \( as text
                    value += "\\(";
                    break;
                }
                case 'u': {
                    // Unicode escape: \u{XXXX}
                    if (match('{')) {
                        uint32_t codepoint = 0;
                        while (pos_ < source_.size() && peek() != '}') {
                            char h = advance();
                            codepoint <<= 4;
                            if (isHexDigit(h)) {
                                codepoint += (h >= 'a') ? (h - 'a' + 10) :
                                             (h >= 'A') ? (h - 'A' + 10) : (h - '0');
                            }
                        }
                        match('}'); // consume closing brace
                        // Encode as UTF-8
                        if (codepoint < 0x80) {
                            value += static_cast<char>(codepoint);
                        } else if (codepoint < 0x800) {
                            value += static_cast<char>(0xC0 | (codepoint >> 6));
                            value += static_cast<char>(0x80 | (codepoint & 0x3F));
                        } else if (codepoint < 0x10000) {
                            value += static_cast<char>(0xE0 | (codepoint >> 12));
                            value += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
                            value += static_cast<char>(0x80 | (codepoint & 0x3F));
                        } else {
                            value += static_cast<char>(0xF0 | (codepoint >> 18));
                            value += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
                            value += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
                            value += static_cast<char>(0x80 | (codepoint & 0x3F));
                        }
                    }
                    break;
                }
                default:
                    value += esc;
                    break;
            }
        } else if (c == '\n') {
            // Newline in single-line string is an error
            return makeErrorToken("unterminated string literal", startPos);
        } else {
            advance();
            value += c;
        }
    }

    return makeErrorToken("unterminated string literal", startPos);
}

// ─── Multiline string scanning ────────────────────────────────────────────

Token Lexer::scanMultilineString() {
    uint32_t startPos = pos_ - 3; // include opening """
    std::string value;

    while (pos_ < source_.size()) {
        if (peek() == '"' && peekAt(1) == '"' && peekAt(2) == '"') {
            advance(); advance(); advance(); // closing """
            Token tok = makeToken(TokenKind::StringLiteral, startPos);
            tok.literalKind = LiteralKind::String;
            tok.stringValue = std::move(value);
            return tok;
        }
        char c = advance();
        if (c == '\\') {
            if (pos_ < source_.size()) {
                char esc = advance();
                switch (esc) {
                    case 'n':  value += '\n'; break;
                    case 't':  value += '\t'; break;
                    case '\\': value += '\\'; break;
                    case '"':  value += '"'; break;
                    default:   value += esc; break;
                }
            }
        } else {
            value += c;
        }
    }

    return makeErrorToken("unterminated multiline string literal", startPos);
}

// ─── Raw string scanning ──────────────────────────────────────────────────

Token Lexer::scanRawString() {
    uint32_t startPos = pos_ - 2; // include r"
    std::string value;

    while (pos_ < source_.size()) {
        char c = peek();
        if (c == '"') {
            advance(); // closing "
            Token tok = makeToken(TokenKind::StringLiteral, startPos);
            tok.literalKind = LiteralKind::String;
            tok.stringValue = std::move(value);
            return tok;
        }
        if (c == '\n') {
            return makeErrorToken("unterminated raw string literal", startPos);
        }
        advance();
        value += c; // no escape processing in raw strings
    }

    return makeErrorToken("unterminated raw string literal", startPos);
}

// ─── Character literal scanning ───────────────────────────────────────────

Token Lexer::scanChar() {
    uint32_t startPos = pos_ - 1; // include opening '
    uint32_t codepoint = 0;

    if (peek() == '\\') {
        advance(); // skip backslash
        char esc = advance();
        switch (esc) {
            case 'n':  codepoint = '\n'; break;
            case 't':  codepoint = '\t'; break;
            case 'r':  codepoint = '\r'; break;
            case '\\': codepoint = '\\'; break;
            case '\'': codepoint = '\''; break;
            case '0':  codepoint = '\0'; break;
            case 'u': {
                if (match('{')) {
                    codepoint = 0;
                    while (pos_ < source_.size() && peek() != '}') {
                        char h = advance();
                        codepoint <<= 4;
                        if (isHexDigit(h)) {
                            codepoint += (h >= 'a') ? (h - 'a' + 10) :
                                         (h >= 'A') ? (h - 'A' + 10) : (h - '0');
                        }
                    }
                    match('}');
                }
                break;
            }
            default:
                codepoint = esc;
                break;
        }
    } else {
        codepoint = decodeUTF8();
    }

    if (!match('\'')) {
        return makeErrorToken("expected closing single quote for character literal", startPos);
    }

    Token tok = makeToken(TokenKind::CharLiteral, startPos);
    tok.literalKind = LiteralKind::Char;
    tok.literal.charValue = codepoint;
    return tok;
}

// ─── Identifier scanning ──────────────────────────────────────────────────

Token Lexer::scanIdentifier() {
    uint32_t startPos = pos_;

    while (pos_ < source_.size()) {
        char c = peek();
        if (isIdentContinue(c)) {
            advance();
        } else if ((unsigned char)c > 0x7F) {
            // Unicode character — accept as identifier continuation
            decodeUTF8();
        } else {
            break;
        }
    }

    std::string_view name(&source_[startPos], pos_ - startPos);

    // Check if it's a raw string prefix
    if (name == "r" && peek() == '"') {
        advance(); // skip the "
        return scanRawString();
    }

    // Check for keyword
    TokenKind kwKind = Token::keywordLookup(name);
    if (kwKind != TokenKind::Identifier) {
        return makeToken(kwKind, startPos);
    }

    Token tok = makeToken(TokenKind::Identifier, startPos);
    tok.stringValue = std::string(name);
    return tok;
}

// ─── Attribute scanning ──────────────────────────────────────────────────

Token Lexer::scanAttribute() {
    uint32_t startPos = pos_ - 1; // include @

    // If @ is not followed by an identifier character, it's just the @ token
    if (!isIdentStart(peek())) {
        return makeToken(TokenKind::At, startPos);
    }

    // Scan the attribute name
    while (pos_ < source_.size() && (isIdentContinue(peek()) || peek() == '.')) {
        advance();
    }

    std::string_view attrName(&source_[startPos], pos_ - startPos);

    // Special case: @enum(C) — peek ahead for the parenthetical
    if (attrName == "@enum" && peek() == '(') {
        advance(); // skip (
        if (peek() == 'C') {
            advance(); // skip C
            if (match(')')) {
                return makeToken(TokenKind::AtEnumC, startPos);
            }
        }
        // If not @enum(C), backtrack is hard — just treat as generic attribute
        // For simplicity, we'll handle it differently in the parser
    }

    // Check for known attributes
    TokenKind kind = Token::attributeLookup(attrName);
    if (kind != TokenKind::AtAttribute) {
        return makeToken(kind, startPos);
    }

    return makeToken(TokenKind::AtAttribute, startPos);
}

// ─── Comment scanning ─────────────────────────────────────────────────────

Token Lexer::scanLineComment() {
    uint32_t startPos = pos_;
    // Skip //
    advance(); advance();

    bool isDoc = false;
    if (peek() == '/') {
        // /// doc comment
        isDoc = true;
        advance();
    }

    while (pos_ < source_.size() && peek() != '\n') {
        advance();
    }
    // At this point, pos_ should be at '\n' or at end of source.
    // Don't consume the '\n' — skipWhitespace() will handle it.

    // Line comments are skipped (not returned as tokens).
    // Return an error token as a placeholder; skipWhitespace() ignores the return value.
    return makeToken(TokenKind::Error, startPos);
}

Token Lexer::scanBlockComment() {
    uint32_t startPos = pos_;
    // Skip /*
    advance(); advance();

    bool isDoc = false;
    if (peek() == '*') {
        // /** doc comment */
        isDoc = true;
        advance();
    }

    int depth = 1;
    while (pos_ < source_.size() && depth > 0) {
        if (peek() == '/' && peekAt(1) == '*') {
            depth++;
            advance(); advance();
        } else if (peek() == '*' && peekAt(1) == '/') {
            depth--;
            advance(); advance();
        } else {
            advance();
        }
    }

    if (depth > 0) {
        return makeErrorToken("unterminated block comment", startPos);
    }

    // Block comments are skipped (not returned as tokens).
    // Return an error token as a placeholder; skipWhitespace() ignores the return value.
    return makeToken(TokenKind::Error, startPos);
}

// ─── UTF-8 decoding ──────────────────────────────────────────────────────

uint32_t Lexer::decodeUTF8() {
    if (pos_ >= source_.size()) return 0;

    unsigned char c = static_cast<unsigned char>(source_[pos_]);
    uint32_t codepoint = 0;
    int bytes = 0;

    if (c < 0x80) {
        codepoint = c;
        bytes = 1;
    } else if ((c & 0xE0) == 0xC0) {
        codepoint = c & 0x1F;
        bytes = 2;
    } else if ((c & 0xF0) == 0xE0) {
        codepoint = c & 0x0F;
        bytes = 3;
    } else if ((c & 0xF8) == 0xF0) {
        codepoint = c & 0x07;
        bytes = 4;
    } else {
        // Invalid UTF-8 lead byte
        advance();
        return 0xFFFD; // replacement character
    }

    advance(); // consume lead byte

    for (int i = 1; i < bytes; i++) {
        if (pos_ >= source_.size()) break;
        unsigned char b = static_cast<unsigned char>(source_[pos_]);
        if ((b & 0xC0) != 0x80) break;
        codepoint = (codepoint << 6) | (b & 0x3F);
        advance();
    }

    return codepoint;
}

} // namespace suki
