#pragma once
// SukiLSP - SukiCode Language Server Protocol 实现
// LSP server for IDE integration.

#include "SymbolIndex.h"
#include <string>
#include <functional>
#include <unordered_map>
#include <vector>

namespace suki::lsp {

// LSP 消息 / LSP message
struct Message {
    std::string method;
    std::string id;
    std::string params;
};

// 位置 / Position
struct Position {
    int line = 0;
    int character = 0;
};

// 范围 / Range
struct Range {
    Position start;
    Position end;
};

// 文本编辑 / Text edit
struct TextEdit {
    Range range;
    std::string newText;
};

// 诊断信息 / Diagnostic
struct Diagnostic {
    Range range;
    int severity = 1; // 1=Error, 2=Warning, 3=Info, 4=Hint
    std::string message;
    std::string source = "sukic";
};

// 补全项 / Completion item
struct CompletionItem {
    std::string label;
    int kind = 0; // LSP completion item kind
    std::string detail;
    std::string documentation;
};

// 文档 / Document
struct Document {
    std::string uri;
    std::string content;
    int version = 0;
};

// LSP 服务器 / LSP Server
class LSPServer {
public:
    LSPServer();
    ~LSPServer();

    // 运行服务器 / Run server
    void run();

private:
    // ─── 消息处理 / Message handling ────────────────────────────────────
    void processMessage(const Message& msg);
    std::string createResponse(const std::string& id, const std::string& result);
    std::string createError(const std::string& id, int code, const std::string& message);
    void sendNotification(const std::string& method, const std::string& params);
    void publishDiagnostics(const std::string& uri, const std::vector<Diagnostic>& diagnostics);

    // ─── LSP 方法 / LSP methods ─────────────────────────────────────────
    std::string handleInitialize(const std::string& id, const std::string& params);
    std::string handleInitialized(const std::string& id, const std::string& params);
    std::string handleShutdown(const std::string& id, const std::string& params);
    std::string handleDidOpen(const std::string& params);
    std::string handleDidChange(const std::string& params);
    std::string handleDidClose(const std::string& params);
    std::string handleCompletion(const std::string& id, const std::string& params);
    std::string handleDefinition(const std::string& id, const std::string& params);
    std::string handleHover(const std::string& id, const std::string& params);

    // ─── 分析 / Analysis ────────────────────────────────────────────────
    void analyzeDocument(const std::string& uri);
    std::vector<Diagnostic> diagnose(const std::string& content, const std::string& uri);
    std::vector<CompletionItem> getCompletions(const std::string& uri, Position pos);

    // ─── IO ─────────────────────────────────────────────────────────────
    std::string readMessage();
    void sendMessage(const std::string& message);

    // ─── 状态 / State ──────────────────────────────────────────────────
    std::unordered_map<std::string, Document> documents_;
    SymbolIndex symbolIndex_;
    bool running_ = true;
};

} // namespace suki::lsp
