// Lexer unit tests for SukiCode.
// Tests tokenization of all token types.

#include "compiler/lexer/Lexer.h"
#include "compiler/lexer/Token.h"
#include "compiler/diag/Diagnostic.h"

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

using namespace suki;

// Helper: lex source and return tokens (excluding Eof)
static std::vector<Token> lex(const std::string& source) {
    DiagnosticEngine diag;
    Lexer lexer(source, "test.suki", diag);
    auto tokens = lexer.lexAll();
    // Remove Eof
    if (!tokens.empty() && tokens.back().is(TokenKind::Eof)) {
        tokens.pop_back();
    }
    return tokens;
}

static void testKeywords() {
    auto tokens = lex("let var func return if else while for in struct class enum");
    assert(tokens.size() == 12);
    assert(tokens[0].is(TokenKind::KwLet));
    assert(tokens[1].is(TokenKind::KwVar));
    assert(tokens[2].is(TokenKind::KwFunc));
    assert(tokens[3].is(TokenKind::KwReturn));
    assert(tokens[4].is(TokenKind::KwIf));
    assert(tokens[5].is(TokenKind::KwElse));
    assert(tokens[6].is(TokenKind::KwWhile));
    assert(tokens[7].is(TokenKind::KwFor));
    assert(tokens[8].is(TokenKind::KwIn));
    assert(tokens[9].is(TokenKind::KwStruct));
    assert(tokens[10].is(TokenKind::KwClass));
    assert(tokens[11].is(TokenKind::KwEnum));
    std::cout << "PASS: testKeywords\n";
}

static void testIdentifiers() {
    auto tokens = lex("foo bar_baz _count");
    assert(tokens.size() == 3);
    assert(tokens[0].is(TokenKind::Identifier));
    assert(tokens[0].stringValue == "foo");
    assert(tokens[1].is(TokenKind::Identifier));
    assert(tokens[1].stringValue == "bar_baz");
    assert(tokens[2].is(TokenKind::Identifier));
    assert(tokens[2].stringValue == "_count");
    std::cout << "PASS: testIdentifiers\n";
}

static void testIntegerLiterals() {
    auto tokens = lex("42 0xFF 0b1010 1_000_000");
    assert(tokens.size() == 4);
    assert(tokens[0].is(TokenKind::IntegerLiteral));
    assert(tokens[0].literal.intValue == 42);
    assert(tokens[1].is(TokenKind::IntegerLiteral));
    assert(tokens[1].literal.intValue == 0xFF);
    assert(tokens[2].is(TokenKind::IntegerLiteral));
    assert(tokens[2].literal.intValue == 10);
    assert(tokens[3].is(TokenKind::IntegerLiteral));
    assert(tokens[3].literal.intValue == 1000000);
    std::cout << "PASS: testIntegerLiterals\n";
}

static void testFloatLiterals() {
    auto tokens = lex("3.14 1.0e10 2.5f");
    assert(tokens.size() == 3);
    assert(tokens[0].is(TokenKind::FloatLiteral));
    assert(tokens[0].literal.floatValue == 3.14);
    assert(tokens[1].is(TokenKind::FloatLiteral));
    assert(tokens[1].literal.floatValue == 1.0e10);
    assert(tokens[2].is(TokenKind::FloatLiteral));
    std::cout << "PASS: testFloatLiterals\n";
}

static void testStringLiterals() {
    DiagnosticEngine diag;
    Lexer lexer("\"hello world\"", "test.suki", diag);
    auto tok = lexer.next();
    assert(tok.is(TokenKind::StringLiteral));
    assert(tok.stringValue == "hello world");
    std::cout << "PASS: testStringLiterals\n";
}

static void testOperators() {
    auto tokens = lex("+ - * / % == != < > <= >= && || ! & | ^ ~ << >> += -= *= /= <-");
    assert(tokens[0].is(TokenKind::Plus));
    assert(tokens[1].is(TokenKind::Minus));
    assert(tokens[2].is(TokenKind::Star));
    assert(tokens[3].is(TokenKind::Slash));
    assert(tokens[4].is(TokenKind::Percent));
    assert(tokens[5].is(TokenKind::Equal));
    assert(tokens[6].is(TokenKind::NotEqual));
    assert(tokens[7].is(TokenKind::Less));
    assert(tokens[8].is(TokenKind::Greater));
    assert(tokens[9].is(TokenKind::LessEqual));
    assert(tokens[10].is(TokenKind::GreaterEqual));
    assert(tokens[11].is(TokenKind::AmpAmp));
    assert(tokens[12].is(TokenKind::PipePipe));
    assert(tokens[13].is(TokenKind::Bang));
    assert(tokens[14].is(TokenKind::Amp));
    assert(tokens[15].is(TokenKind::Pipe));
    assert(tokens[16].is(TokenKind::Caret));
    assert(tokens[17].is(TokenKind::Tilde));
    assert(tokens[18].is(TokenKind::LShift));
    assert(tokens[19].is(TokenKind::RShift));
    assert(tokens[20].is(TokenKind::PlusAssign));
    assert(tokens[21].is(TokenKind::MinusAssign));
    assert(tokens[22].is(TokenKind::StarAssign));
    assert(tokens[23].is(TokenKind::SlashAssign));
    assert(tokens[24].is(TokenKind::LeftArrow));
    std::cout << "PASS: testOperators\n";
}

static void testPunctuation() {
    auto tokens = lex("( ) { } [ ] . , : ; @ # -> => _ ... ..<");
    assert(tokens[0].is(TokenKind::LParen));
    assert(tokens[1].is(TokenKind::RParen));
    assert(tokens[2].is(TokenKind::LBrace));
    assert(tokens[3].is(TokenKind::RBrace));
    assert(tokens[4].is(TokenKind::LBracket));
    assert(tokens[5].is(TokenKind::RBracket));
    assert(tokens[6].is(TokenKind::Dot));
    assert(tokens[7].is(TokenKind::Comma));
    assert(tokens[8].is(TokenKind::Colon));
    assert(tokens[9].is(TokenKind::Semicolon));
    assert(tokens[10].is(TokenKind::At));
    assert(tokens[11].is(TokenKind::Hash));
    assert(tokens[12].is(TokenKind::Arrow));
    assert(tokens[13].is(TokenKind::FatArrow));
    assert(tokens[14].is(TokenKind::Underscore));
    assert(tokens[15].is(TokenKind::Ellipsis));
    assert(tokens[16].is(TokenKind::Range));
    std::cout << "PASS: testPunctuation\n";
}

static void testComments() {
    // Single-line comment: should be skipped
    auto tokens = lex("let x = 10 // this is a comment\nvar y = 20");
    assert(tokens.size() == 8);
    assert(tokens[0].is(TokenKind::KwLet));
    assert(tokens[1].is(TokenKind::Identifier) && tokens[1].stringValue == "x");
    assert(tokens[2].is(TokenKind::Assign));
    assert(tokens[3].is(TokenKind::IntegerLiteral));
    assert(tokens[4].is(TokenKind::KwVar));
    assert(tokens[5].is(TokenKind::Identifier) && tokens[5].stringValue == "y");
    assert(tokens[6].is(TokenKind::Assign));
    assert(tokens[7].is(TokenKind::IntegerLiteral));

    // Block comment
    auto tokens2 = lex("let /* comment */ x = 1");
    assert(tokens2.size() == 4);
    assert(tokens2[0].is(TokenKind::KwLet));
    assert(tokens2[1].is(TokenKind::Identifier) && tokens2[1].stringValue == "x");
    assert(tokens2[2].is(TokenKind::Assign));
    assert(tokens2[3].is(TokenKind::IntegerLiteral));

    std::cout << "PASS: testComments\n";
}

static void testLineNumbers() {
    auto tokens = lex("let x = 10\nvar y = 20\nlet z = 30");
    assert(tokens[0].loc.line == 1);
    assert(tokens[4].loc.line == 2); // var
    assert(tokens[8].loc.line == 3); // let z
    std::cout << "PASS: testLineNumbers\n";
}

int main() {
    testKeywords();
    testIdentifiers();
    testIntegerLiterals();
    testFloatLiterals();
    testStringLiterals();
    testOperators();
    testPunctuation();
    testComments();
    testLineNumbers();

    std::cout << "\nAll lexer tests passed!\n";
    return 0;
}
