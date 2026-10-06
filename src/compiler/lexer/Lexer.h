#pragma once

#include "compiler/diag/DiagnosticEngine.h"
#include "compiler/lexer/Token.h"
#include <string>
#include <vector>

namespace suki {

// Lexer turns a source string into a stream of tokens.
// Design notes:
//  - Trivia (whitespace, // and /* */ comments) is skipped; doc comments
//    (/// and /** */) are accumulated and attached to the following token.
//  - String interpolation "\(expr)" is tokenized as
//        TK_StringFragment, <expr tokens>, TK_StringFragment, TK_StringEnd
//    via an interpolation-depth state machine that supports nesting.
//  - A plain (non-interpolated) string is emitted as a single TK_StringLiteral.
class Lexer {
public:
    Lexer(std::string source, DiagnosticEngine& diags);

    // 设定条件编译（`os(...)` / `arch(...)`，规范 §10.3）所用的目标三元组。
    // 未显式设定时回退到主机目标，因此默认行为与 `sukic run`（本机执行）一致。
    void setTargetTriple(std::string triple) { targetTriple_ = std::move(triple); }

    // 规范 §10.3：通过 `-D NAME[=VALUE]` 注入的自定义宏，参与条件编译求值。
    // 形如 `-D FLAG`（无值）等价于定义为空串；`-D LEVEL=5` 定义为 "5"，
    // 可在 `#if` 中按数值比较（见 dCondPrimary）。
    void setDefines(std::vector<std::string> defs) { userDefines_ = std::move(defs); }

    // Tokenize the whole input (EOF token included at the end).
    std::vector<Token> tokenizeAll();

    // Produce one token at a time.
    Token next();

private:
    void advance();
    char peek(size_t off = 0) const;
    bool atEnd() const;
    static bool isIdentStart(char c);
    static bool isIdentCont(char c);
    static bool isDigit(char c);

    void skipTrivia();
    Token lexSingle();
    Token lexIdentifierOrKeyword();
    Token lexNumber();
    Token lexString(bool raw, bool multiline);
    Token lexStringBody(bool raw, bool multiline);
    Token finishString(const std::string& fragment, size_t closePos, size_t quoteLen);
    Token resumeString();
    Token lexChar();
    Token lexPunctuator();
    Token makeError(const std::string& msg);
    Token makeToken(TokenKind kind);
    Token stringEndToken();

    std::string source_;
    std::string targetTriple_;     // 条件编译目标（空 = 主机目标）
    std::vector<std::string> userDefines_; // 规范 §10.3：`-D NAME[=VALUE]` 注入的宏
    size_t pos_   = 0;
    size_t line_  = 1;
    size_t col_   = 1;
    DiagnosticEngine& diags_;

    std::string pendingDoc_;       // accumulated doc comment for next token
    int interpDepth_     = 0;      // >0 while inside "\( ... )"
    bool pendingStringEnd_ = false; // a TK_StringEnd is queued
    bool interpolatedString_ = false; // current string uses interpolation
    bool curRaw_   = false;        // current string is a raw string
    bool curMultiline_ = false;    // current string is a multiline string
    size_t curOpenStart_ = 0;      // opening quote byte offset
    SourceLocation curOpenLoc_;     // opening quote location
};

} // namespace suki
