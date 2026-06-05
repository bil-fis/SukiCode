// Parser unit tests for SukiCode.
// Tests parsing of declarations, statements, and expressions.

#include "compiler/lexer/Lexer.h"
#include "compiler/parser/Parser.h"
#include "compiler/ast/ASTNode.h"
#include "compiler/ast/ASTPrinter.h"
#include "compiler/diag/Diagnostic.h"

#include <cassert>
#include <iostream>
#include <string>

using namespace suki;

static std::unique_ptr<CompilationUnit> parse(const std::string& source) {
    DiagnosticEngine diag;
    Lexer lexer(source, "test.suki", diag);
    auto tokens = lexer.lexAll();
    Parser parser(std::move(tokens), source, "test.suki", diag);
    auto cu = parser.parse();

    if (diag.hadErrors()) {
        std::cerr << "Parse errors:\n";
        diag.printAll(source, "test.suki");
    }

    return cu;
}

static void testModuleDeclaration() {
    auto cu = parse("module MyApp");
    assert(cu != nullptr);
    assert(cu->moduleDecl != nullptr);
    assert(cu->moduleDecl->name == "MyApp");
    std::cout << "PASS: testModuleDeclaration\n";
}

static void testVariableDeclaration() {
    auto cu = parse("let x = 42");
    assert(cu != nullptr);
    assert(cu->declarations.size() >= 1);
    // First decl might be module or variable
    Decl* varDecl = nullptr;
    for (auto& d : cu->declarations) {
        if (d->declKind == DeclKind::Variable) {
            varDecl = d.get();
            break;
        }
    }
    assert(varDecl != nullptr);
    auto* vd = static_cast<VariableDecl*>(varDecl);
    assert(vd->isLet == true);
    std::cout << "PASS: testVariableDeclaration\n";
}

static void testFunctionDeclaration() {
    auto cu = parse("func add(_ a: Int, _ b: Int) -> Int {\n    return a + b\n}");
    assert(cu != nullptr);
    Decl* funcDecl = nullptr;
    for (auto& d : cu->declarations) {
        if (d->declKind == DeclKind::Function) {
            funcDecl = d.get();
            break;
        }
    }
    assert(funcDecl != nullptr);
    auto* fd = static_cast<FunctionDecl*>(funcDecl);
    assert(fd->name == "add");
    assert(fd->params.size() == 2);
    std::cout << "PASS: testFunctionDeclaration\n";
}

static void testStructDeclaration() {
    auto cu = parse("struct Point {\n    var x: Double\n    var y: Double\n}");
    assert(cu != nullptr);
    Decl* structDecl = nullptr;
    for (auto& d : cu->declarations) {
        if (d->declKind == DeclKind::Struct) {
            structDecl = d.get();
            break;
        }
    }
    assert(structDecl != nullptr);
    auto* sd = static_cast<StructDecl*>(structDecl);
    assert(sd->name == "Point");
    assert(sd->members.size() == 2);
    std::cout << "PASS: testStructDeclaration\n";
}

static void testClassDeclaration() {
    auto cu = parse("class Animal {\n    var name: String\n    func speak() -> String {\n        return \"...\"\n    }\n}");
    assert(cu != nullptr);
    Decl* classDecl = nullptr;
    for (auto& d : cu->declarations) {
        if (d->declKind == DeclKind::Class) {
            classDecl = d.get();
            break;
        }
    }
    assert(classDecl != nullptr);
    auto* cd = static_cast<ClassDecl*>(classDecl);
    assert(cd->name == "Animal");
    std::cout << "PASS: testClassDeclaration\n";
}

static void testIfStatement() {
    auto cu = parse("if x > 0 {\n    print(x)\n} else {\n    print(0)\n}");
    assert(cu != nullptr);
    Decl* ifDecl = nullptr;
    for (auto& d : cu->declarations) {
        if (d->declKind == DeclKind::If) {
            ifDecl = d.get();
            break;
        }
    }
    assert(ifDecl != nullptr);
    std::cout << "PASS: testIfStatement\n";
}

static void testForInLoop() {
    auto cu = parse("for item in array {\n    print(item)\n}");
    assert(cu != nullptr);
    Decl* forDecl = nullptr;
    for (auto& d : cu->declarations) {
        if (d->declKind == DeclKind::ForIn) {
            forDecl = d.get();
            break;
        }
    }
    assert(forDecl != nullptr);
    std::cout << "PASS: testForInLoop\n";
}

static void testSwitchStatement() {
    auto cu = parse("switch value {\ncase 0:\n    print(\"zero\")\ncase 1:\n    print(\"one\")\ndefault:\n    print(\"other\")\n}");
    assert(cu != nullptr);
    Decl* switchDecl = nullptr;
    for (auto& d : cu->declarations) {
        if (d->declKind == DeclKind::Switch) {
            switchDecl = d.get();
            break;
        }
    }
    assert(switchDecl != nullptr);
    auto* sd = static_cast<SwitchDecl*>(switchDecl);
    assert(sd->cases.size() == 3);
    std::cout << "PASS: testSwitchStatement\n";
}

static void testEnumDeclaration() {
    auto cu = parse("enum Color {\n    case red\n    case green\n    case blue\n}");
    assert(cu != nullptr);
    Decl* enumDecl = nullptr;
    for (auto& d : cu->declarations) {
        if (d->declKind == DeclKind::Enum) {
            enumDecl = d.get();
            break;
        }
    }
    assert(enumDecl != nullptr);
    auto* ed = static_cast<EnumDecl*>(enumDecl);
    assert(ed->name == "Color");
    assert(ed->cases.size() == 3);
    std::cout << "PASS: testEnumDeclaration\n";
}

static void testHelloWorld() {
    // A complete hello world program
    auto cu = parse(R"(
@main
func main() {
    print("Hello, SukiCode!")
}
)");
    assert(cu != nullptr);
    // Should have at least the main function
    bool foundMain = false;
    for (auto& d : cu->declarations) {
        if (d->declKind == DeclKind::Function) {
            auto* fd = static_cast<FunctionDecl*>(d.get());
            if (fd->name == "main") foundMain = true;
        }
    }
    assert(foundMain);
    std::cout << "PASS: testHelloWorld\n";
}

static void testASTPrinter() {
    auto cu = parse("let x = 42");
    ASTPrinter printer;
    std::string output = printer.print(*cu);
    assert(!output.empty());
    std::cout << "PASS: testASTPrinter\n";
}

int main() {
    testModuleDeclaration();
    testVariableDeclaration();
    testFunctionDeclaration();
    testStructDeclaration();
    testClassDeclaration();
    testIfStatement();
    testForInLoop();
    testSwitchStatement();
    testEnumDeclaration();
    testHelloWorld();
    testASTPrinter();

    std::cout << "\nAll parser tests passed!\n";
    return 0;
}
