#pragma once
// 符号表和作用域管理
// Symbol table and scope management for SukiCode semantic analysis.

#include "Type.h"
#include "compiler/ast/ASTNode.h"
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>

namespace suki {

// 符号种类 / Symbol kinds
enum class SymbolKind : uint8_t {
    Variable,
    Function,
    Type,
    EnumCase,
    Module,
};

// 符号信息 / Symbol information
struct Symbol {
    SymbolKind kind;
    std::string name;
    TypePtr type;
    bool isConstant = false;     // let vs var
    bool isPublic = false;
    bool isInitialized = false;
    AccessLevel access = AccessLevel::Internal;
    std::string declaringFile;   // 声明所在文件
    int scopeDepth = 0;          // 声明时的作用域深度

    // 函数特有 / Function-specific
    std::vector<TypePtr> paramTypes;
    TypePtr returnType;

    // 源位置 / Source location
    uint32_t line = 0;
    uint32_t column = 0;
};

// 作用域 / Scope
class Scope {
public:
    explicit Scope(std::shared_ptr<Scope> parent = nullptr)
        : parent_(std::move(parent)) {}

    // 在当前作用域定义符号 / Define a symbol in this scope
    bool define(const Symbol& symbol);

    // 查找符号（向上搜索父作用域）/ Look up a symbol (search parent scopes)
    Symbol* lookup(const std::string& name);

    // 仅在当前作用域查找 / Look up only in this scope
    Symbol* lookupLocal(const std::string& name);

    // 获取父作用域 / Get parent scope
    std::shared_ptr<Scope> parent() const { return parent_; }

    // 获取所有本地符号 / Get all local symbols
    const std::unordered_map<std::string, Symbol>& symbols() const { return symbols_; }

private:
    std::shared_ptr<Scope> parent_;
    std::unordered_map<std::string, Symbol> symbols_;
};

// 符号表 / Symbol table
class SymbolTable {
public:
    SymbolTable();

    // 进入新作用域 / Enter a new scope
    void enterScope();

    // 离开当前作用域 / Leave the current scope
    void leaveScope();

    // 定义符号 / Define a symbol
    bool define(const Symbol& symbol);

    // 查找符号 / Look up a symbol
    Symbol* lookup(const std::string& name);

    // 获取当前作用域 / Get current scope
    std::shared_ptr<Scope> currentScope() const { return currentScope_; }

    // 作用域深度 / Scope depth
    int depth() const { return depth_; }

    // 设置当前文件 / Set current file
    void setCurrentFile(const std::string& file) { currentFile_ = file; }
    const std::string& currentFile() const { return currentFile_; }

    // 检查访问权限 / Check access permission
    bool isAccessible(const Symbol& sym) const;

private:
    std::shared_ptr<Scope> currentScope_;
    int depth_ = 0;
    std::string currentFile_;
};

} // namespace suki
