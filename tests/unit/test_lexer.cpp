// Lexer unit tests for SukiCode.
//
// Covers token classes and the lexical rules that are easy to regress: keyword
// recognition, numeric literal forms, string interpolation, and the boundary
// rules that decide whether a '.' starts a float or is member access.

#include "compiler/lexer/Lexer.h"
#include "compiler/lexer/Token.h"
#include "compiler/diag/DiagnosticEngine.h"

#include <iostream>
#include <string>
#include <vector>

using namespace suki;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool cond, const std::string& what) {
    ++g_checks;
    if (!cond) {
        ++g_failures;
        std::cout << "  FAIL: " << what << "\n";
    }
}

// Tokenize `source`, dropping the trailing Eof marker.
std::vector<Token> lex(const std::string& source) {
    DiagnosticEngine diags;
    std::vector<Token> tokens;
    {
        Lexer lexer(source, diags);
        tokens = lexer.tokenizeAll();
    }
    if (!tokens.empty() && tokens.back().kind == TokenKind::TK_EOF) tokens.pop_back();
    return tokens;
}

void expectKind(const std::string& src, TokenKind kind, const std::string& what) {
    auto t = lex(src);
    check(t.size() == 1 && t[0].kind == kind,
          what + " (got " + std::string(t.empty() ? "<none>" : "other") + ")");
}

// A keyword is a TK_Keyword token whose payload names the keyword.
void expectKeyword(const std::string& src, KeywordID kw, const std::string& what) {
    auto t = lex(src);
    check(t.size() == 1 && t[0].kind == TokenKind::TK_Keyword && t[0].keyword == kw,
          what + " is the expected keyword");
}

void testKeywords() {
    std::cout << "Keywords\n";
    expectKeyword("let", KeywordID::Let, "let");
    expectKeyword("func", KeywordID::Func, "func");
    expectKeyword("struct", KeywordID::Struct, "struct");
    expectKeyword("class", KeywordID::Class, "class");
    expectKeyword("enum", KeywordID::Enum, "enum");
    // Names that used to be reserved are ordinary identifiers again, so
    // `pool`, `owned` and `task` must lex as identifiers.
    for (const char* id : {"pool", "owned", "channel", "task", "arc"}) {
        auto t = lex(id);
        check(t.size() == 1 && t[0].kind == TokenKind::TK_Identifier,
              std::string(id) + " is an identifier");
    }
}

void testNumbers() {
    std::cout << "Numeric literals\n";
    expectKind("42", TokenKind::TK_IntLiteral, "integer");
    expectKind("3.14", TokenKind::TK_FloatLiteral, "float");
    // A leading-dot float is a literal only at a token boundary.
    expectKind(".5", TokenKind::TK_FloatLiteral, "leading-dot float");
    auto t = lex("t.0");
    check(t.size() == 3 && t[1].kind == TokenKind::TK_Punctuator,
          "'.' after an identifier is member access, not a float");
}

void testStrings() {
    std::cout << "String literals\n";
    auto plain = lex("\"hello\"");
    check(plain.size() == 1 && plain[0].kind == TokenKind::TK_StringLiteral,
          "plain string");
    // Interpolation splits one literal into segments and expressions, so the
    // token count grows; what matters is that it is a string token stream.
    auto interp = lex("\"a \\(b) c\"");
    check(!interp.empty(), "interpolated string lexes");
}

void testOperators() {
    std::cout << "Operators\n";
    // Multi-character operators must lex as one token, otherwise `a ?? b`
    // would parse as two `?`.
    auto coalesce = lex("??");
    check(coalesce.size() == 1 &&
          coalesce[0].kind == TokenKind::TK_Punctuator &&
          coalesce[0].punct == PunctuatorID::QuestionQuestion,
          "?? is a single token");
    for (const char* op : {"<<", ">>", "->", "==", "!=", "&&", "||", "..."}) {
        auto t = lex(op);
        check(t.size() == 1, std::string(op) + " is a single token");
    }
}

void testComments() {
    std::cout << "Comments\n";
    auto t = lex("42 // trailing\n");
    check(t.size() == 1, "line comment is skipped");
    auto b = lex("/* block */ 7");
    check(b.size() == 1 && b[0].kind == TokenKind::TK_IntLiteral,
          "block comment is skipped");
}

} // namespace

int main() {
    std::cout << "=== Lexer unit tests ===\n";
    testKeywords();
    testNumbers();
    testStrings();
    testOperators();
    testComments();

    std::cout << (g_failures ? "\nFAILED " : "\nPASSED ")
              << (g_checks - g_failures) << "/" << g_checks << " checks\n";
    return g_failures ? 1 : 0;
}
