#pragma once
// SukiCode recursive descent parser.
// Transforms a token stream into an AST (CompilationUnit).

#include "compiler/lexer/Lexer.h"
#include "compiler/lexer/Token.h"
#include "compiler/ast/ASTNode.h"
#include "compiler/diag/Diagnostic.h"
#include <vector>
#include <string_view>

namespace suki {

class Parser {
public:
    Parser(std::vector<Token> tokens, std::string_view source,
           std::string_view filename, DiagnosticEngine& diag);

    // Parse the entire source file into a CompilationUnit
    std::unique_ptr<CompilationUnit> parse();

private:
    // ─── Token navigation ─────────────────────────────────────────────────
    const Token& peek() const;
    const Token& peekAt(size_t offset) const;
    const Token& advance();
    bool check(TokenKind kind) const;
    bool match(TokenKind kind);
    bool expect(TokenKind kind); // consume if match, else error
    bool isAtEnd() const;
    SourceLocation loc() const;

    // ─── Declarations ─────────────────────────────────────────────────────
    DeclPtr parseDeclaration();
    // Parse declaration and collect any extra decls (multi-variable) into the given vector
    void parseDeclarationInto(std::vector<DeclPtr>& out);
    DeclPtr parseModuleDecl();
    DeclPtr parseImportDecl();
    DeclPtr parseVariableDecl();  // let / var
    DeclPtr parseFunctionDecl();  // func
    DeclPtr parseStructDecl();
    DeclPtr parseClassDecl();
    DeclPtr parseEnumDecl();
    DeclPtr parseProtocolDecl();
    DeclPtr parseActorDecl();
    DeclPtr parseExtensionDecl();
    DeclPtr parseTypealiasDecl();
    DeclPtr parseAssociatedTypeDecl();
    DeclPtr parseInitDecl();
    DeclPtr parseDeinitDecl();
    DeclPtr parseSubscriptDecl();

    // Control flow
    DeclPtr parseIfDecl();
    DeclPtr parseGuardDecl();
    DeclPtr parseSwitchDecl();
    DeclPtr parseForInDecl();
    DeclPtr parseWhileDecl();
    DeclPtr parseRepeatWhileDecl();
    DeclPtr parseDoCatchDecl();
    DeclPtr parseSelectDecl();
    DeclPtr parseUnsafeDecl();
    DeclPtr parseAsmDecl();
    DeclPtr parseMacroDecl(MacroKind kind);
    DeclPtr parseExternDecl();
    DeclPtr parseExternFuncDecl(const std::string& callingConv);
    FunctionParam parseExternParam();

    // ─── Statements ───────────────────────────────────────────────────────
    StmtPtr parseStatement();
    StmtPtr parseReturnStmt();
    StmtPtr parseBreakStmt();
    StmtPtr parseContinueStmt();
    StmtPtr parseFallthroughStmt();
    StmtPtr parseDeferStmt();
    StmtPtr parseThrowStmt();
    std::vector<StmtPtr> parseBlock(); // { stmts }

    // ─── Expressions ──────────────────────────────────────────────────────
    // Precedence climbing for binary operators
    ExprPtr parseExpression();
    ExprPtr parseAssignmentExpr();
    ExprPtr parseTernaryExpr();
    ExprPtr parseRangeExpr();
    ExprPtr parseLogicalOrExpr();
    ExprPtr parseLogicalAndExpr();
    ExprPtr parseBitwiseOrExpr();
    ExprPtr parseBitwiseXorExpr();
    ExprPtr parseBitwiseAndExpr();
    ExprPtr parseComparisonExpr();
    ExprPtr parseShiftExpr();
    ExprPtr parseAdditionExpr();
    ExprPtr parseMultiplicationExpr();
    ExprPtr parsePrefixExpr();
    ExprPtr parsePostfixExpr();
    ExprPtr parsePrimaryExpr();

    ExprPtr parseCallExpr(ExprPtr callee);
    ExprPtr parseMemberAccessExpr(ExprPtr base);
    ExprPtr parseSubscriptExpr(ExprPtr base);

    // ─── Types ────────────────────────────────────────────────────────────
    TypeReprPtr parseType();
    TypeReprPtr parseSimpleType();
    TypeReprPtr parseFunctionType();

    // ─── Patterns ─────────────────────────────────────────────────────────
    PatternPtr parsePattern();

    // ─── Function parameters ──────────────────────────────────────────────
    FunctionParam parseFunctionParam();
    std::vector<FunctionParam> parseParamList();

    // ─── Generic parameters ───────────────────────────────────────────────
    std::vector<GenericParam> parseGenericParams();

    // ─── Access control ───────────────────────────────────────────────────
    AccessLevel parseAccessLevel();

    // ─── Helpers ──────────────────────────────────────────────────────────
    // Error recovery: skip to next statement boundary
    void synchronize();
    void error(std::string_view message);

    // ─── State ────────────────────────────────────────────────────────────
    std::vector<Token> tokens_;
    size_t pos_;
    std::string_view source_;
    std::string_view filename_;
    DiagnosticEngine& diag_;
    NodeID nextNodeId_ = 1;
    uint64_t uniqueIdCounter_ = 0; // 用于 #unique 生成唯一标识符
    // Multi-variable declarations: var x, y: Double produces extra decls
    std::vector<DeclPtr> multiDecls_;

    template<typename T>
    std::unique_ptr<T> makeNode() {
        auto node = std::make_unique<T>();
        node->id = nextNodeId_++;
        return node;
    }
};

} // namespace suki
