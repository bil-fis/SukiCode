#pragma once
// SukiCode Lexer
// Scans a UTF-8 source string into a stream of tokens.
// Handles string interpolation, multi-line strings, numeric literals,
// and all SukiCode operators/punctuation.

#include "Token.h"
#include "compiler/diag/Diagnostic.h"
#include <string>
#include <string_view>
#include <vector>

namespace suki {

class Lexer {
public:
    Lexer(std::string_view source, std::string_view filename, DiagnosticEngine& diag);
    ~Lexer();

    // Lex the entire source into a token vector (includes Eof)
    std::vector<Token> lexAll();

    // Lex a single token
    Token next();

private:
    // ─── Source navigation ─────────────────────────────────────────────────
    char peek() const;         // current char (0 at end)
    char peekAt(uint32_t offset) const; // peek ahead
    char advance();            // consume and return current char
    void skipWhitespace();
    bool match(char expected); // consume if current == expected

    // Unicode-aware: decode a UTF-8 code point at current position
    uint32_t decodeUTF8();

    // ─── Token scanning ───────────────────────────────────────────────────
    Token scanToken();
    Token scanNumber();        // integer or float literal
    Token scanString();        // "..." with interpolation
    Token scanMultilineString(); // """..."""
    Token scanRawString();     // r"..." (no escapes)
    Token scanChar();          // '...'
    Token scanIdentifier();    // identifier or keyword
    Token scanAttribute();     // @identifier
    Token scanLineComment();   // //...
    Token scanBlockComment();  // /* ... */
    Token scanOperator();      // multi-char operators

    // ─── String interpolation ─────────────────────────────────────────────
    // \(expr) inside string literals requires nested parsing.
    // The lexer handles this by returning the string-so-far as a token,
    // then the parser will handle the interpolated expression.

    // ─── Helpers ──────────────────────────────────────────────────────────
    bool isDigit(char c) const { return c >= '0' && c <= '9'; }
    bool isHexDigit(char c) const;
    bool isAlpha(char c) const;
    bool isAlphaNumeric(char c) const;
    bool isIdentStart(char c) const;
    bool isIdentContinue(char c) const;

    // Create a token with the given kind spanning from startPos to current pos
    Token makeToken(TokenKind kind, uint32_t startPos);
    Token makeErrorToken(std::string_view message, uint32_t startPos);

    // ─── Conditional compilation ───────────────────────────────────────────
    bool evaluateCondition(); // 评估 #if 条件
    void skipUntilHashEnd();  // 跳过到 #endif
    void skipUntilHashEndOrNext(); // 跳过到 #elseif/#else/#endif

    // ─── State ────────────────────────────────────────────────────────────
    std::string_view source_;
    std::string_view filename_;
    uint32_t pos_;          // current byte offset
    uint32_t line_;         // current line (1-based)
    uint32_t lineStart_;    // byte offset of current line start
    DiagnosticEngine& diag_;
    std::vector<std::string> defines_; // 已定义的编译标志
};

} // namespace suki
