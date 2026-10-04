// Semantic-analysis unit tests for SukiCode.
//
// Each case drives the real front end (Lexer -> Parser -> Sema) over a small
// snippet and asserts on whether diagnostics were produced. A regression in name
// resolution, type compatibility or flow analysis therefore fails here, with a
// precise message, instead of surfacing later as a confusing .suki failure.

#include "compiler/ast/AST.h"
#include "compiler/diag/DiagnosticEngine.h"
#include "compiler/lexer/Lexer.h"
#include "compiler/parser/Parser.h"
#include "compiler/sema/Sema.h"

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

// Run the front end and report whether the analyser rejected the snippet.
bool analyzeHasError(const std::string& source) {
    DiagnosticEngine diags;
    {
        Lexer lex(source, diags);
        Parser parser(lex.tokenizeAll(), diags);
        NodeList decls = parser.parseModule();
        Sema sema(diags);
        sema.analyze(decls);
    }
    return diags.hasErrors();
}

// A snippet that must be accepted without any diagnostic.
void accepts(const std::string& name, const std::string& src) {
    check(!analyzeHasError(src), name + ": expected acceptance, got a diagnostic");
}

// A snippet that must be rejected by the analyser.
void rejects(const std::string& name, const std::string& src) {
    check(analyzeHasError(src), name + ": expected a diagnostic, got acceptance");
}

// ── name resolution ────────────────────────────────────────────────────────
void testNameResolution() {
    std::cout << "Name resolution\n";
    accepts("local variable", R"(
        @main
        func main() -> Int {
            let x = 1
            return x
        }
    )");
    rejects("unknown identifier", R"(
        @main
        func main() -> Int { return nope }
    )");
    rejects("call of unknown function", R"(
        @main
        func main() -> Int { return missingFn() }
    )");
}

// ── type compatibility ─────────────────────────────────────────────────────
void testTypeChecking() {
    std::cout << "Type checking\n";
    accepts("integer arithmetic", R"(
        @main
        func main() -> Int { return 1 + 2 }
    )");
    rejects("String returned as Int", R"(
        func f() -> Int { return "text" }
        @main
        func main() -> Int { return f() }
    )");
    rejects("argument type mismatch", R"(
        func takesInt(v: Int) -> Int { return v }
        @main
        func main() -> Int { return takesInt(v: "text") }
    )");
}

// ── flow analysis ──────────────────────────────────────────────────────────
void testFlowAnalysis() {
    std::cout << "Flow analysis\n";
    accepts("if/else both return", R"(
        func f(n: Int) -> Int {
            if n > 0 { return 1 } else { return 2 }
        }
        @main
        func main() -> Int { return f(1) }
    )");
    rejects("missing return", R"(
        func f() -> Int { let x = 1 }
        @main
        func main() -> Int { return 0 }
    )");
    // A switch with a `default` whose every arm returns is exhaustive.
    accepts("exhaustive switch", R"(
        enum E { case a
                 case b }
        func f(e: E) -> Int {
            switch e {
                case E.a: return 1
                default: return 0
            }
        }
        @main
        func main() -> Int { return f(E.a) }
    )");
}

// ── aggregates ─────────────────────────────────────────────────────────────
void testAggregates() {
    std::cout << "Aggregates\n";
    accepts("struct field access", R"(
        struct P { var x: Int
                   var y: Int }
        @main
        func main() -> Int {
            let p = P(x: 1, y: 2)
            return p.x
        }
    )");
    rejects("tuple index out of range", R"(
        @main
        func main() -> Int {
            let t = (1, 2)
            return t.5
        }
    )");
    rejects("enum case arity", R"(
        enum S { case c(r: Int)
                 case e }
        @main
        func main() -> Int {
            let s = S.c(1, 2)
            return 0
        }
    )");
    // Payload bindings introduce names scoped to their own arm.
    accepts("enum payload binding", R"(
        enum S { case c(r: Int)
                 case e }
        func area(s: S) -> Int {
            switch s {
                case S.c(let r): return r
                default: return 0
            }
        }
        @main
        func main() -> Int { return area(S.c(3)) }
    )");
}

// ── classes ────────────────────────────────────────────────────────────────
void testClasses() {
    std::cout << "Classes\n";
    accepts("class construction and methods", R"(
        class C {
            var v: Int
            init(v: Int) { self.v = v }
            func get() -> Int { return self.v }
        }
        @main
        func main() -> Int {
            let c = C(v: 1)
            return c.get()
        }
    )");
}

// ── collections ────────────────────────────────────────────────────────────
void testCollections() {
    std::cout << "Collections\n";
    accepts("array literal and count", R"(
        @main
        func main() -> Int {
            let a = [1, 2, 3]
            return a.count
        }
    )");
    // An empty literal carries no element to infer from, so the annotation must
    // supply the type; lowering depends on it for the element stride.
    accepts("empty array with annotation", R"(
        @main
        func main() -> Int {
            let a: [Int] = []
            return a.count
        }
    )");
    accepts("empty dictionary with annotation", R"(
        @main
        func main() -> Int {
            let d: [Int: Int] = [:]
            return d.count
        }
    )");
}

} // namespace

int main() {
    std::cout << "=== Sema unit tests ===\n";
    testNameResolution();
    testTypeChecking();
    testFlowAnalysis();
    testAggregates();
    testClasses();
    testCollections();

    std::cout << (g_failures ? "\nFAILED " : "\nPASSED ")
              << (g_checks - g_failures) << "/" << g_checks << " checks\n";
    return g_failures ? 1 : 0;
}
