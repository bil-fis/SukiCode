#pragma once
// SukiCode 宏展开引擎
// Macro expansion engine with hygiene and sandboxing.

#include "compiler/ast/ASTNode.h"
#include "compiler/diag/Diagnostic.h"
#include <string>
#include <unordered_map>
#include <set>

namespace suki {

// 宏定义 / Macro definition
struct MacroInfo {
    std::string name;
    MacroKind kind = MacroKind::Freestanding;
    size_t paramCount = 0;
    const MacroDecl* sourceDecl = nullptr;
    SourceLocation loc;
};

// 卫生宏的符号前缀管理 / Hygiene symbol prefix management
class HygieneContext {
public:
    std::string uniqueName(const std::string& base) {
        return "__macro_" + std::to_string(nextId_++) + "_" + base;
    }

    std::string uniqueId(const std::string& base) {
        return base + "$" + std::to_string(nextId_++);
    }

    void enterMacro(const std::string& macroName) {
        prefixStack_.push_back("__" + macroName + "_" + std::to_string(nextId_++) + "_");
    }

    void leaveMacro() {
        if (!prefixStack_.empty()) prefixStack_.pop_back();
    }

private:
    uint64_t nextId_ = 0;
    std::vector<std::string> prefixStack_;
};

// 沙箱环境 / Sandbox environment
class MacroSandbox {
public:
    static bool isAllowed(const std::string& operation) {
        static const std::set<std::string> blocked = {
            "file_read", "file_write", "file_open", "file_delete",
            "net_connect", "net_send", "net_recv",
            "process_create", "process_exec",
            "syscall", "system", "popen",
            "env_get", "env_set",
            "thread_create", "thread_spawn",
            "mmap", "munmap",
            "dlopen", "dlsym",
            "signal", "sigaction",
            "fork", "exec", "waitpid",
            "socket", "bind", "listen", "accept", "connect",
            "open", "read", "write", "close", "ioctl",
            "malloc", "free", "realloc", "calloc",
        };
        return blocked.find(operation) == blocked.end();
    }

    static bool containsBlockedOperation(const Stmt& stmt) {
        if (stmt.stmtKind == StmtKind::Expression) {
            auto& es = static_cast<const ExpressionStmt&>(stmt);
            if (es.expression && es.expression->exprKind == ExprKind::Call) {
                auto& call = static_cast<const CallExpr&>(*es.expression);
                if (call.callee->exprKind == ExprKind::Identifier) {
                    auto& id = static_cast<const IdentifierExpr&>(*call.callee);
                    if (!isAllowed(id.name)) return true;
                }
            }
        }
        return false;
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
        info.sourceDecl = &decl;
        info.loc = decl.loc;
        macros_[decl.name] = info;
    }

    // 展开宏 / Expand macro
    // 返回展开后的声明，失败返回 nullptr
    const MacroDecl* expandMacro(const MacroExpansionExpr& expr) {
        auto it = macros_.find(expr.macroName);
        if (it == macros_.end()) {
            diag_.error(expr.loc, "", "undefined macro: " + expr.macroName);
            return nullptr;
        }

        const MacroInfo& macro = it->second;

        // 检查参数数量 / Check argument count
        if (expr.args.size() != macro.paramCount) {
            diag_.error(expr.loc, "", "macro '" + expr.macroName + "' expects " +
                std::to_string(macro.paramCount) + " arguments, got " +
                std::to_string(expr.args.size()));
            return nullptr;
        }

        // 沙箱检查 / Sandbox check
        if (macro.sourceDecl) {
            for (const auto& stmt : macro.sourceDecl->body) {
                if (stmt && MacroSandbox::containsBlockedOperation(*stmt)) {
                    diag_.error(expr.loc, "", "macro '" + expr.macroName + "' contains blocked operation");
                    return nullptr;
                }
            }
        }

        // 进入卫生上下文 / Enter hygiene context
        hygiene_.enterMacro(expr.macroName);
        hygiene_.leaveMacro();

        return macro.sourceDecl;
    }

    bool isMacroDefined(const std::string& name) const {
        return macros_.find(name) != macros_.end();
    }

    size_t macroCount() const { return macros_.size(); }

    HygieneContext& hygiene() { return hygiene_; }

private:
    DiagnosticEngine& diag_;
    std::unordered_map<std::string, MacroInfo> macros_;
    HygieneContext hygiene_;
};

} // namespace suki
