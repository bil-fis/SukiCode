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
