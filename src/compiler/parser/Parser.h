#pragma once

#include "compiler/ast/AST.h"
#include "compiler/diag/DiagnosticEngine.h"
#include "compiler/lexer/Token.h"
#include <vector>

namespace suki {

// Recursive-descent parser producing the AST defined in ast/AST.h.
// Includes error recovery: on a parse error it reports a diagnostic and
// resynchronizes to the next statement/declaration boundary instead of
// aborting, so multiple errors can be reported in one pass.
class Parser {
public:
    Parser(std::vector<Token> tokens, DiagnosticEngine& diags);

    // Parse an entire translation unit; returns top-level declarations.
    NodeList parseModule();

    // Parse a single expression (used by tests / REPL).
    NodePtr parseExpression();

private:
    // ── token stream ───────────────────────────────────────────────────────
    const Token& cur() const;
    const Token& peek(size_t off = 1) const;
    bool atEnd() const;
    bool check(TokenKind k) const;
    bool checkPunct(PunctuatorID p) const;
    bool checkKw(KeywordID k) const;
    bool match(TokenKind k);
    bool matchPunct(PunctuatorID p);
    bool matchKw(KeywordID k);
    Token advance();
    Token expect(TokenKind k, const char* msg);
    Token expectPunct(PunctuatorID p, const char* msg);
    Token expectKw(KeywordID k, const char* msg);
    void errorAt(const Token& t, const std::string& msg);
    void synchronize();

    // ── declarations ─────────────────────────────────────────────────────────
    NodePtr parseDecl();
    std::vector<std::string> parseModifiers();
    NodePtr parseFunctionDecl(std::vector<std::string> modifiers);
    NodePtr parseInitializerDecl(std::vector<std::string> modifiers);
    NodePtr parseDeinitializerDecl();
    NodePtr parseSubscriptDecl(std::vector<std::string> modifiers);
    NodePtr parseTypeDecl(NodeKind kind, std::vector<std::string> modifiers);
    NodePtr parseTypealiasDecl();
    NodePtr parseAssociatedTypeDecl();
    NodePtr parseVarDecl(bool isLet, std::vector<std::string> modifiers, bool member);
    // Parse a computed-property accessor block: `{ get {..} set {..} }` or an
    // implicit getter `{ statements }`. Returns the collected statements.
    std::vector<NodePtr> parseAccessors();
    // `{1, 2, 3}` set literal; the opening brace is already current.
    NodePtr parseSetLiteral();
    // True when the tokens after `{` form a closure parameter list.
    bool closureParamsAhead();
    // True when `[` at the cursor opens a capture list (`[weak self] in ...`).
    bool captureListAhead();
    // True when the token after the current one begins an expression.
    bool nextStartsExpression();
    NodePtr parseEnumCase();
    std::vector<Param> parseParameterList();
    std::vector<std::string> parseGenericParamNames(); // <T, U: Bound>
    void parseWhereConstraints();          // where T: Proto, U == V
    NodeList parseInheritedTypes();        // : A, B, C

    // ── statements ────────────────────────────────────────────────────────────
    NodePtr parseStatement();
    NodePtr parseBlock();
    NodeList parseBlockStatements();
    NodePtr parseIfStmt();
    NodePtr parseGuardStmt();
    NodePtr parseWhileStmt();
    NodePtr parseRepeatStmt();
    NodePtr parseForInStmt();
    NodePtr parseSwitchStmt();
    NodePtr parseSelectStmt();
    NodePtr parseReturnStmt();
    NodePtr parseThrowStmt();
    NodePtr parseDeferStmt();
    NodePtr parseDoStmt();
    NodePtr parseUnsafeStmt();
    NodePtr parseCondition();

    // ── expressions (precedence climbing) ─────────────────────────────────────
    NodePtr parseExpr();
    // Parse an expression in a context where a following `{` is a statement
    // block (if/while/guard conditions, for-in sequences, switch subjects),
    // not a trailing closure.
    NodePtr parseExprNoTrailingClosure();
    // Parse a case-pattern subject, stopping before a payload binding list so
    // that `case Shape.circle(let r)` is not read as a call taking `let r`.
    // Binding names are appended to `bindings`.
    NodePtr parseCasePatternExpr(std::vector<std::string>& bindings);
    NodePtr parseAssignment();
    // A conditional whose condition is already parsed (then-branch of `?:`).
    NodePtr parseConditionalFromRange();
    NodePtr parseRange();
    NodePtr parseNilCoalescing();
    NodePtr parseLogicalOr();
    NodePtr parseLogicalAnd();
    NodePtr parseBitwiseOr();
    NodePtr parseBitwiseXor();
    NodePtr parseBitwiseAnd();
    NodePtr parseComparison();
    NodePtr parseShift();
    NodePtr parseAdditive();
    NodePtr parseMultiplicative();
    NodePtr parseUnary();
    NodePtr parsePostfix();
    NodePtr parsePrimary();
    NodePtr parseCallSuffix(NodePtr callee);
    NodePtr parseClosure(bool arrowSyntax);
    NodePtr parseCollectionLiteral();
    bool looksLikeArrowClosure();

    // ── types ──────────────────────────────────────────────────────────────────
    NodePtr parseType();
    NodePtr parseTypePostfix(NodePtr base);
    // 泛型实参列表（`<` 已消费）：成功返回持有各实参的 TupleType，失败返回
    // nullptr 且不消费 `>`，由调用方决定回退。
    NodePtr parseTypeArgsUntilGreater();

    std::vector<Token> tokens_;
    size_t pos_ = 0;
    DiagnosticEngine& diags_;
    // >0 while parsing a control-flow condition/subject, where `{` denotes a
    // block rather than a trailing closure argument.
    int suppressTrailingClosure_ = 0;
    // >0 while parsing a case pattern, where `(` starts a payload binding list
    // rather than a call argument list.
    int suppressCall_ = 0;
    // 泛型约束累积区：parseGenericParamNames / parseWhereConstraints 解析到的
    // 约束暂存于此，调用方随后取走填入 decl->genericConstraints（规范 2.1）。
    std::vector<GenericConstraint> pendingGenericConstraints_;
    // @_cdecl("name") 捕获的外部符号名暂存区（规范 6.3）。
    std::string pendingCdeclName_;
    // @enum(C) 捕获的 C 兼容枚举标记暂存区（规范 1.5）。
    bool pendingCEnum_ = false;
};

} // namespace suki
