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

} // namespace suki
