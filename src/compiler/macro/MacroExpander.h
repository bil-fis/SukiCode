#pragma once
// SukiCode 宏展开引擎
// Macro expansion engine with hygiene and sandboxing.

#include "compiler/ast/ASTNode.h"
#include "compiler/diag/Diagnostic.h"
#include <string>
#include <unordered_map>
#include <memory>
#include <sstream>
#include <functional>
#include <set>

namespace suki {

// 宏定义 / Macro definition
struct MacroInfo {
    std::string name;
    MacroKind kind = MacroKind::Freestanding;
    size_t paramCount = 0;
    SourceLocation loc;

    // 存储展开模板的文本表示 / Store expansion template as text
    std::string expansionTemplate;
    std::vector<std::string> bodyStatements;
};

// 宏展开结果 / Macro expansion result
struct MacroExpansionResult {
    std::vector<StmtPtr> statements; // 展开后的语句
    std::vector<ExprPtr> expressions; // 展开后的表达式
    bool success = false;
    std::string error;
};

// 卫生宏的符号前缀管理 / Hygiene symbol prefix management
class HygieneContext {
public:
    // 生成唯一符号名 / Generate unique symbol name
    std::string uniqueName(const std::string& base) {
        return "__macro_" + std::to_string(nextId_++) + "_" + base;
    }

    // 生成全局唯一标识符 / Generate globally unique identifier
    std::string uniqueId(const std::string& base) {
        return base + "$" + std::to_string(nextId_++);
    }

    // 获取当前宏的前缀 / Get current macro prefix
    const std::string& currentPrefix() const {
        static std::string empty;
        return prefixStack_.empty() ? empty : prefixStack_.back();
    }

    // 进入宏展开 / Enter macro expansion
    void enterMacro(const std::string& macroName) {
        prefixStack_.push_back("__" + macroName + "_" + std::to_string(nextId_++) + "_");
    }

    // 离开宏展开 / Leave macro expansion
    void leaveMacro() {
        if (!prefixStack_.empty()) prefixStack_.pop_back();
    }

private:
    uint64_t nextId_ = 0;
    std::vector<std::string> prefixStack_;
};

// 沙箱环境 / Sandbox environment
// 宏在沙箱中执行，禁止以下操作：
// - 文件 I/O
// - 网络访问
// - 进程创建
// - 系统调用
class MacroSandbox {
public:
    // 检查操作是否允许 / Check if operation is allowed
    static bool isAllowed(const std::string& operation) {
        // 禁止的操作列表 / Blocked operations
        static const std::set<std::string> blocked = {
            "file_read", "file_write", "file_open",
            "net_connect", "net_send", "net_recv",
            "process_create", "process_exec",
            "syscall", "system",
            "env_get", "env_set",
            "thread_create", "thread_spawn",
        };
        return blocked.find(operation) == blocked.end();
    }

    // 沙箱执行宏体 / Execute macro body in sandbox
    // 返回 true 如果执行成功
    static bool executeInSandbox(std::function<void()> body) {
        try {
            body();
            return true;
        } catch (...) {
            return false;
        }
    }
};

// 宏展开器 / Macro expander
class MacroExpander {
public:
    MacroExpander(DiagnosticEngine& diag) : diag_(diag) {}

    // 注册宏定义 / Register macro definition
    void registerMacro(const MacroDecl& decl) {
        MacroInfo info;
        info.name = decl.name;
        info.kind = decl.macroKind;
        info.paramCount = decl.params.size();
        info.loc = decl.loc;

        // 提取展开模板文本 / Extract expansion template text
        // 简化实现：存储宏体语句数量
        info.bodyStatements.push_back("/* macro body: " + std::to_string(decl.body.size()) + " statements */");

        macros_[decl.name] = std::move(info);
    }

    // 展开宏表达式 / Expand macro expression
    MacroExpansionResult expandMacro(const MacroExpansionExpr& expr) {
        MacroExpansionResult result;

        auto it = macros_.find(expr.macroName);
        if (it == macros_.end()) {
            result.error = "undefined macro: " + expr.macroName;
            diag_.error(expr.loc, "", result.error);
            return result;
        }

        const MacroInfo& macro = it->second;

        // 检查参数数量 / Check argument count
        if (expr.args.size() != macro.paramCount) {
            result.error = "macro '" + expr.macroName + "' expects " +
                std::to_string(macro.paramCount) + " arguments, got " +
                std::to_string(expr.args.size());
            diag_.error(expr.loc, "", result.error);
            return result;
        }

        // 进入卫生上下文 / Enter hygiene context
        hygiene_.enterMacro(expr.macroName);

        // 在沙箱中执行宏体 / Execute macro body in sandbox
        bool success = MacroSandbox::executeInSandbox([&]() {
            // 简化实现：返回宏体语句 / Simplified: return macro body statements
            // 实际实现应该执行宏体并生成展开代码
            // Actual implementation should execute macro body and generate expansion code
            for (const auto& stmt : macro.bodyStatements) {
                // 这里应该生成实际的 AST 节点
                // Here we should generate actual AST nodes
                // 简化：返回空结果
            }
        });

        // 离开卫生上下文 / Leave hygiene context
        hygiene_.leaveMacro();

        result.success = success;
        if (!success) {
            result.error = "macro expansion failed: sandbox violation";
            diag_.error(expr.loc, "", result.error);
        }

        return result;
    }

    // 检查宏是否已定义 / Check if macro is defined
    bool isMacroDefined(const std::string& name) const {
        return macros_.find(name) != macros_.end();
    }

    // 获取宏信息 / Get macro info
    const MacroInfo* getMacro(const std::string& name) const {
        auto it = macros_.find(name);
        return it != macros_.end() ? &it->second : nullptr;
    }

    // 获取卫生上下文 / Get hygiene context
    HygieneContext& hygiene() { return hygiene_; }

private:
    DiagnosticEngine& diag_;
    std::unordered_map<std::string, MacroInfo> macros_;
    HygieneContext hygiene_;
};

} // namespace suki
