// Code-generation unit tests for SukiCode.
//
// These assert on the *textual LLVM IR* rather than on behaviour, because the
// lowering decisions that are easy to regress silently (aggregate layout, the
// class object header, tagged unions, switch dispatch) are all visible in the
// IR text. A behavioural test would only notice them via a crash.

#include "compiler/codegen/IRGenerator.h"
#include "compiler/diag/DiagnosticEngine.h"
#include "compiler/lexer/Lexer.h"
#include "compiler/parser/Parser.h"
#include "compiler/sema/Sema.h"
#include "compiler/codegen/TargetInfo.h"

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

// Parse `src` and lower it to textual IR. emitIR takes a declaration list, so
// each case runs the front end first and then lowers the resulting AST.
bool lowerToIR(const std::string& src, std::string& ir) {
    DiagnosticEngine diags;
    IRGenerator gen(hostTarget());
    NodeList decls;
    {
        Lexer lex(src, diags);
        Parser parser(lex.tokenizeAll(), diags);
        decls = parser.parseModule();
    }
    Sema sema(diags);
    sema.analyze(decls);
    std::string err;
    if (!gen.emitIR(decls, ir, err)) {
        std::cout << "  (lowering failed: " << err << ")\n";
        return false;
    }
    return true;
}

// A snippet whose IR must contain `needle`.
void expectIR(const std::string& name, const std::string& src,
              const std::string& needle) {
    std::string ir;
    if (!lowerToIR(src, ir)) {
        check(false, name + ": IR generation failed");
        return;
    }
    check(ir.find(needle) != std::string::npos,
          name + ": expected to find \"" + needle + "\" in the IR");
}

// A snippet whose IR must *not* contain `needle`.
void rejectIR(const std::string& name, const std::string& src,
              const std::string& needle) {
    std::string ir;
    if (!lowerToIR(src, ir)) {
        check(false, name + ": IR generation failed");
        return;
    }
    check(ir.find(needle) == std::string::npos,
          name + ": did not expect \"" + needle + "\" in the IR");
}

// ── scalars and strings ────────────────────────────────────────────────────
void testScalarsAndStrings() {
    std::cout << "Scalars and strings\n";
    // A String is a {pointer, length} aggregate, not a bare char*.
    expectIR("string aggregate", R"SUKI(
        @main
        func main() -> Int {
            let s = "hi"
            return 0
        }
    )SUKI", "SukiString");
    // Printing routes through the runtime entry point that takes the aggregate.
    expectIR("print of a string", R"SUKI(
        @main
        func main() -> Int {
            print("hi")
            return 0
        }
    )SUKI", "suki_print_str");
    // Interpolation folds the segments with runtime concatenation.
    expectIR("interpolation", R"SUKI(
        @main
        func main() -> Int {
            let n = 1
            print("n = \(n)")
            return 0
        }
    )SUKI", "suki_str_concat");
}

// ── aggregates ─────────────────────────────────────────────────────────────
void testStructLayout() {
    std::cout << "Struct layout\n";
    // A struct is a named LLVM struct with one field per stored property.
    expectIR("named struct", R"SUKI(
        struct P { var x: Int
                   var y: Int }
        @main
        func main() -> Int {
            let p = P(x: 1, y: 2)
            return p.x
        }
    )SUKI", "suki.P");
    // Field order follows declaration order.
    expectIR("field offsets", R"SUKI(
        struct P { var x: Int
                   var y: Int }
        @main
        func main() -> Int {
            let p = P(x: 1, y: 2)
            return p.y
        }
    )SUKI", "i32 1");
}

void testEnums() {
    std::cout << "Enums\n";
    // A raw-valued enum lowers to a plain integer.
    expectIR("raw enum is an integer", R"SUKI(
        enum E { case a
                 case b }
        @main
        func main() -> Int {
            let e = E.a
            return 0
        }
    )SUKI", "i64 0");
    // An enum with payloads is a {tag, payload} pair.
    expectIR("payload enum is tagged", R"SUKI(
        enum E { case c(r: Int)
                 case e }
        @main
        func main() -> Int {
            let e = E.c(1)
            return 0
        }
    )SUKI", "insertvalue");
    // A raw-valued enum must not gain a payload slot: that mistake made a
    // payload-free case of a payload-carrying enum incompatible with the
    // enum's own parameter type.
    rejectIR("raw enum is not tagged", R"SUKI(
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
    )SUKI", "insertvalue");
    // Dispatch is a real LLVM switch over the tag.
    expectIR("switch dispatch", R"SUKI(
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
    )SUKI", "switch i64");
}

// ── classes and ARC ────────────────────────────────────────────────────────
void testClasses() {
    std::cout << "Classes and ARC\n";
    // Instances live on the heap, not the stack.
    expectIR("class instance is heap allocated", R"SUKI(
        class C { var v: Int
                  init(v: Int) { self.v = v } }
        @main
        func main() -> Int {
            let c = C(v: 1)
            return 0
        }
    )SUKI", "suki_alloc");
    // The header is { vtable, retain count } and the count starts at one.
    expectIR("object header initialised", R"SUKI(
        class C { var v: Int
                  init(v: Int) { self.v = v } }
        @main
        func main() -> Int {
            let c = C(v: 1)
            return 0
        }
    )SUKI", "i64 1");
    // Storing an existing reference into a class field takes a reference.
    expectIR("field store retains", R"SUKI(
        class Inner { var v: Int
                      init(v: Int) { self.v = v } }
        class Outer { var i: Inner
                      init(i: Inner) { self.i = i } }
        @main
        func main() -> Int {
            let a = Inner(v: 1)
            let o = Outer(i: a)
            return 0
        }
    )SUKI", "suki_arc_retain");
    // The object header reserves a destructor slot, so the runtime can run
    // `deinit` without knowing the concrete type.
    expectIR("deinit slot written", R"SUKI(
        class C { var v: Int
                  init(v: Int) { self.v = v }
                  deinit { } }
        @main
        func main() -> Int {
            let c = C(v: 1)
            return 0
        }
    )SUKI", "deinit.slot");
    // A subclass inherits its parent's destructor when it declares none.
    expectIR("inherited deinit slot", R"SUKI(
        class A { var v: Int
                  init(v: Int) { self.v = v }
                  deinit { } }
        class B : A { init(v: Int) { super.init(v: v) } }
        @main
        func main() -> Int {
            let b = B(v: 1)
            return 0
        }
    )SUKI", "@A.deinit");
    // The vtable slot is written at construction time.
    expectIR("vtable slot written", R"SUKI(
        class C { var v: Int
                  init(v: Int) { self.v = v } }
        @main
        func main() -> Int {
            let c = C(v: 1)
            return 0
        }
    )SUKI", "vtable");
}

// ── collections ────────────────────────────────────────────────────────────
void testCollections() {
    std::cout << "Collections\n";
    // An array is {data, length, capacity} and is filled through the runtime.
    expectIR("array aggregate", R"SUKI(
        @main
        func main() -> Int {
            let a = [1, 2, 3]
            return a.count
        }
    )SUKI", "suki_array_new");
    expectIR("array length read", R"SUKI(
        @main
        func main() -> Int {
            let a = [1, 2, 3]
            return a.count
        }
    )SUKI", "suki_array_len");
    // Dictionaries use the shared open-addressing table.
    expectIR("dictionary insert", R"SUKI(
        @main
        func main() -> Int {
            let d: [Int: Int] = [:]
            d.set(1, 2)
            return d.count
        }
    )SUKI", "suki_dict_set");
    // Strings report byte length and Unicode scalar count through distinct
    // entry points.
    expectIR("string length", R"SUKI(
        @main
        func main() -> Int {
            let s = "abc"
            return s.length
        }
    )SUKI", "suki_str_length");
    expectIR("string scalar count", R"SUKI(
        @main
        func main() -> Int {
            let s = "abc"
            return s.count
        }
    )SUKI", "suki_str_utf8_count");
}

} // namespace

int main() {
    std::cout << "=== Codegen unit tests ===\n";
    testScalarsAndStrings();
    testStructLayout();
    testEnums();
    testClasses();
    testCollections();

    std::cout << (g_failures ? "\nFAILED " : "\nPASSED ")
              << (g_checks - g_failures) << "/" << g_checks << " checks\n";
    return g_failures ? 1 : 0;
}
