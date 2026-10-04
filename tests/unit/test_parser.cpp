// Parser unit tests for SukiCode.
//
// The parser's job is to build the right tree and to keep going after an error.
// These cases therefore check two things: that valid input produces the expected
// declaration/statement shape, and that malformed input is reported without
// looping forever (a real bug once: a lookahead path re-entered itself and grew
// the token stream until allocation failed).

#include "compiler/lexer/Lexer.h"
#include "compiler/parser/Parser.h"
#include "compiler/ast/AST.h"
#include "compiler/diag/DiagnosticEngine.h"

#include <iostream>
#include <string>

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

struct ParseResult {
    NodeList decls;
    bool hadError = false;
};

ParseResult parseSource(const std::string& source) {
    ParseResult r;
    DiagnosticEngine diags;
    {
        Lexer lex(source, diags);
        Parser parser(lex.tokenizeAll(), diags);
        r.decls = parser.parseModule();
    }
    r.hadError = diags.hasErrors();
    return r;
}

void accepts(const std::string& name, const std::string& src) {
    auto r = parseSource(src);
    check(!r.hadError, name + ": expected acceptance, got a diagnostic");
}

void rejects(const std::string& name, const std::string& src) {
    auto r = parseSource(src);
    check(r.hadError, name + ": expected a diagnostic, got acceptance");
}

void testDeclarations() {
    std::cout << "Declarations\n";
    accepts("function", R"(
        func add(a: Int, b: Int) -> Int { return a + b }
    )");
    accepts("variadic parameter", R"(
        func log(items: Int...) -> Int { return 0 }
    )");
    accepts("struct with methods", R"(
        struct P {
            var x: Int
            func get() -> Int { return self.x }
        }
    )");
    accepts("enum with payloads", R"(
        enum E { case a
                 case c(r: Int) }
    )");
    accepts("class with initialiser", R"(
        class C {
            var v: Int
            init(v: Int) { self.v = v }
        }
    )");
}

void testStatementsAndExpressions() {
    std::cout << "Statements and expressions\n";
    accepts("if/else", "func f() -> Int { if true { return 1 } else { return 2 } }");
    accepts("while", "func f() { while true { break } }");
    accepts("for-in", "func f(xs: [Int]) -> Int { var s = 0\n for x in xs { s += x }\n return s }");
    accepts("switch", R"(
        func f(n: Int) -> Int {
            switch n {
                case 1: return 1
                default: return 0
            }
        }
    )");
    // A case pattern may bind payload values: `case E.c(let r)`.
    accepts("enum payload pattern", R"(
        enum E { case c(r: Int)
                 case e }
        func f(e: E) -> Int {
            switch e {
                case E.c(let r): return r
                default: return 0
            }
        }
    )");
    // Trailing closures and closure parameter lists.
    accepts("trailing closure", "func f() -> Int { let g = { 1 }\n return 0 }");
    accepts("closure with `in` parameters", "func f() -> Int { let g = { x, y in x }\n return 0 }");
    // `t.0` is tuple indexing; the lexer must not swallow `.0` as a float.
    accepts("tuple index", "func f() -> Int { let t = (1, 2)\n return t.0 }");
    accepts("empty dictionary literal", "func f() -> Int { let d = [:]\n return 0 }");
}

void testOperators() {
    std::cout << "Operators\n";
    // `a < b` is a comparison, while `Base<T>` is a generic specialisation;
    // confusing the two used to send the parser into an unbounded loop.
    accepts("comparison is not a specialisation", "func f(a: Int, b: Int) -> Bool { return a < b }");
    accepts("generic specialisation", "func f() -> Int { return g<Int>() }");
    accepts("bitwise and shift", "func f(a: Int, b: Int) -> Int { return (a & b) << 2 }");
    accepts("nil coalescing", "func f(a: Int?) -> Int { return a ?? 0 }");
    accepts("range", "func f() -> Int { var s = 0\n for i in 0..<10 { s += i }\n return s }");
}

void testErrorRecovery() {
    std::cout << "Error recovery\n";
    // Malformed input must be reported, and parsing must terminate.
    rejects("missing body", "func f(");
    rejects("stray token", "func f() -> Int { return } +++ }");
    rejects("unterminated string", "func f() { let s = \"abc }");
    // Recovery: an error inside one declaration must not swallow the next one.
    // `let s = S()` is legal, so the diagnostic has to come from the bad call
    // that follows, and `g` must still be parsed.
    {
        // Member *existence* is Sema's job, not the parser's; the parser only
        // rejects what it cannot read at all. A stray token mid-expression is
        // such a case, and must not prevent the next declaration from parsing.
        auto r = parseSource(R"(
            struct S { var x: Int }
            func f() -> Int {
                let s = S()
                return @@@
            }
            func g() -> Int { return 1 }
        )");
        check(r.hadError, "recovery: stray token is diagnosed");
        int funcs = 0;
        for (auto& d : r.decls)
            if (d && d->kind == NodeKind::FunctionDecl) ++funcs;
        check(funcs == 2, "recovery: the following declaration still parsed");
    }
}

} // namespace

int main() {
    std::cout << "=== Parser unit tests ===\n";
    testDeclarations();
    testStatementsAndExpressions();
    testOperators();
    testErrorRecovery();

    std::cout << (g_failures ? "\nFAILED " : "\nPASSED ")
              << (g_checks - g_failures) << "/" << g_checks << " checks\n";
    return g_failures ? 1 : 0;
}
