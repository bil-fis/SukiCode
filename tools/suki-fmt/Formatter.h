#pragma once
// SukiCode 代码格式化器
// Code formatter that enforces style rules from spec section 1.9.

#include "compiler/ast/ASTNode.h"
#include <string>
#include <sstream>

namespace suki {

// 格式化配置 / Format configuration
struct FormatConfig {
    int indentWidth = 4;         // 缩进空格数
    int lineLength = 100;        // 行长度限制
    bool useSpaces = true;       // 使用空格（否则 tab）
    bool trailingCommas = true;  // 尾随逗号
};

class Formatter {
public:
    Formatter(const FormatConfig& config = FormatConfig());
    ~Formatter();

    // 格式化编译单元 / Format compilation unit
    std::string format(const CompilationUnit& cu);

    // 格式化源代码字符串 / Format source string
    std::string formatSource(const std::string& source, const std::string& filename);

private:
    // ─── 声明格式化 / Declaration formatting ─────────────────────────────
    void formatDecl(const Decl& decl, int indent);
    void formatVariableDecl(const VariableDecl& decl, int indent);
    void formatFunctionDecl(const FunctionDecl& decl, int indent);
    void formatStructDecl(const StructDecl& decl, int indent);
    void formatClassDecl(const ClassDecl& decl, int indent);
    void formatEnumDecl(const EnumDecl& decl, int indent);
    void formatProtocolDecl(const ProtocolDecl& decl, int indent);

    // ─── 语句格式化 / Statement formatting ───────────────────────────────
    void formatStmt(const Stmt& stmt, int indent);
    void formatReturnStmt(const ReturnStmt& stmt, int indent);
    void formatExprStmt(const ExpressionStmt& stmt, int indent);
    void formatCompoundStmt(const CompoundStmt& stmt, int indent);

    // ─── 表达式格式化 / Expression formatting ────────────────────────────
    void formatExpr(const Expr& expr, int indent);

    // ─── 辅助 / Helpers ──────────────────────────────────────────────────
    void emitIndent(int level);
    void newline();
    void space();
    void emit(const std::string& text);

    FormatConfig config_;
    std::ostringstream out_;
    int currentIndent_ = 0;
    bool needNewline_ = false;
};

} // namespace suki
