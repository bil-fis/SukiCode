// 符号表实现
// Symbol table implementation.

#include "SymbolTable.h"

namespace suki {

// ─── Scope ──────────────────────────────────────────────────────────────

bool Scope::define(const Symbol& symbol) {
    // 检查是否已在当前作用域定义 / Check if already defined in this scope
    if (symbols_.count(symbol.name) > 0) {
        return false; // 重复定义 / Duplicate definition
    }
    symbols_[symbol.name] = symbol;
    return true;
}

Symbol* Scope::lookup(const std::string& name) {
    auto it = symbols_.find(name);
    if (it != symbols_.end()) {
        return &it->second;
    }
    if (parent_) {
        return parent_->lookup(name);
    }
    return nullptr;
}

Symbol* Scope::lookupLocal(const std::string& name) {
    auto it = symbols_.find(name);
    if (it != symbols_.end()) {
        return &it->second;
    }
    return nullptr;
}

// ─── SymbolTable ────────────────────────────────────────────────────────

SymbolTable::SymbolTable() {
    currentScope_ = std::make_shared<Scope>();
}

void SymbolTable::enterScope() {
    currentScope_ = std::make_shared<Scope>(currentScope_);
    depth_++;
}

void SymbolTable::leaveScope() {
    if (currentScope_->parent()) {
        currentScope_ = currentScope_->parent();
        depth_--;
    }
}

bool SymbolTable::define(const Symbol& symbol) {
    return currentScope_->define(symbol);
}

Symbol* SymbolTable::lookup(const std::string& name) {
    return currentScope_->lookup(name);
}

bool SymbolTable::isAccessible(const Symbol& sym) const {
    // public: 任何地方可访问 / accessible anywhere
    if (sym.access == AccessLevel::Public) return true;

    // internal: 同模块可访问（简化：同编译单元）/ same module (simplified: same compilation unit)
    if (sym.access == AccessLevel::Internal) return true;

    // fileprivate: 同文件可访问 / same file accessible
    if (sym.access == AccessLevel::FilePrivate) {
        return sym.declaringFile.empty() || sym.declaringFile == currentFile_;
    }

    // private: 同作用域或外层作用域可访问 / same or outer scope accessible
    if (sym.access == AccessLevel::Private) {
        return depth_ >= sym.scopeDepth;
    }

    return true;
}

} // namespace suki
