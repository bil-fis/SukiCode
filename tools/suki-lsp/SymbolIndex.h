#pragma once
// SukiLSP 符号索引 - 用于代码补全和定义跳转
// Symbol index for code completion and go-to-definition.

#include <string>
#include <vector>
#include <unordered_map>

namespace suki::lsp {

// 符号类型 / Symbol types
enum class SymbolKind : uint8_t {
    Function,
    Variable,
    Type,
    EnumCase,
    Parameter,
    Module,
};

// 符号信息 / Symbol information
struct SymbolInfo {
    SymbolKind kind;
    std::string name;
    std::string file;
    uint32_t line;
    uint32_t column;
    std::string typeName; // 类型名称（用于显示）
    std::string documentation;
};

// 符号索引 / Symbol index
class SymbolIndex {
public:
    SymbolIndex() = default;

    // 添加符号 / Add symbol
    void addSymbol(const SymbolInfo& symbol) {
        symbols_.push_back(symbol);
        nameIndex_[symbol.name].push_back(symbols_.size() - 1);
    }

    // 按名称查找符号 / Find symbols by name
    std::vector<const SymbolInfo*> findByName(const std::string& name) const {
        std::vector<const SymbolInfo*> result;
        auto it = nameIndex_.find(name);
        if (it != nameIndex_.end()) {
            for (size_t idx : it->second) {
                result.push_back(&symbols_[idx]);
            }
        }
        return result;
    }

    // 按文件查找符号 / Find symbols by file
    std::vector<const SymbolInfo*> findByFile(const std::string& file) const {
        std::vector<const SymbolInfo*> result;
        for (const auto& sym : symbols_) {
            if (sym.file == file) {
                result.push_back(&sym);
            }
        }
        return result;
    }

    // 查找位置处的符号 / Find symbol at position
    const SymbolInfo* findAtPosition(const std::string& file, uint32_t line, uint32_t column) const {
        for (const auto& sym : symbols_) {
            if (sym.file == file && sym.line == line) {
                return &sym;
            }
        }
        return nullptr;
    }

    // 清除文件的符号 / Clear symbols for a file
    void clearFile(const std::string& file) {
        std::vector<SymbolInfo> remaining;
        for (const auto& sym : symbols_) {
            if (sym.file != file) {
                remaining.push_back(sym);
            }
        }
        symbols_ = std::move(remaining);
        rebuildIndex();
    }

    // 符号数量 / Symbol count
    size_t size() const { return symbols_.size(); }

private:
    void rebuildIndex() {
        nameIndex_.clear();
        for (size_t i = 0; i < symbols_.size(); i++) {
            nameIndex_[symbols_[i].name].push_back(i);
        }
    }

    std::vector<SymbolInfo> symbols_;
    std::unordered_map<std::string, std::vector<size_t>> nameIndex_;
};

} // namespace suki::lsp
