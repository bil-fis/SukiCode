// SukiLSP implementation - Language Server Protocol server.

#include "LSPServer.h"
#include "compiler/lexer/Lexer.h"
#include "compiler/parser/Parser.h"
#include "compiler/diag/Diagnostic.h"

#include <iostream>
#include <sstream>
#include <algorithm>

namespace suki::lsp {

LSPServer::LSPServer() = default;
LSPServer::~LSPServer() = default;

void LSPServer::run() {
    while (running_) {
        std::string raw = readMessage();
        if (raw.empty()) continue;

        // Parse JSON-RPC message
        // Simple parser: look for "method" and "id" fields
        Message msg;

        // Extract method
        size_t methodPos = raw.find("\"method\"");
        if (methodPos != std::string::npos) {
            size_t start = raw.find('"', methodPos + 8);
            size_t end = raw.find('"', start + 1);
            if (start != std::string::npos && end != std::string::npos) {
                msg.method = raw.substr(start + 1, end - start - 1);
            }
        }

        // Extract id
        size_t idPos = raw.find("\"id\"");
        if (idPos != std::string::npos) {
            size_t start = raw.find_first_of("0123456789\"", idPos + 4);
            if (start != std::string::npos) {
                if (raw[start] == '"') {
                    size_t end = raw.find('"', start + 1);
                    msg.id = raw.substr(start + 1, end - start - 1);
                } else {
                    size_t end = raw.find_first_not_of("0123456789", start);
                    msg.id = raw.substr(start, end - start);
                }
            }
        }

        // Extract params (everything after "params":)
        size_t paramsPos = raw.find("\"params\"");
        if (paramsPos != std::string::npos) {
            msg.params = raw.substr(paramsPos + 8);
        }

        processMessage(msg);
    }
}

void LSPServer::processMessage(const Message& msg) {
    if (msg.method == "initialize") {
        sendMessage(handleInitialize(msg.id, msg.params));
    } else if (msg.method == "initialized") {
        handleInitialized(msg.id, msg.params);
    } else if (msg.method == "shutdown") {
        sendMessage(handleShutdown(msg.id, msg.params));
    } else if (msg.method == "exit") {
        running_ = false;
    } else if (msg.method == "textDocument/didOpen") {
        handleDidOpen(msg.params);
    } else if (msg.method == "textDocument/didChange") {
        handleDidChange(msg.params);
    } else if (msg.method == "textDocument/didClose") {
        handleDidClose(msg.params);
    } else if (msg.method == "textDocument/completion") {
        sendMessage(handleCompletion(msg.id, msg.params));
    } else if (msg.method == "textDocument/definition") {
        sendMessage(handleDefinition(msg.id, msg.params));
    } else if (msg.method == "textDocument/hover") {
        sendMessage(handleHover(msg.id, msg.params));
    }
}

std::string LSPServer::createResponse(const std::string& id, const std::string& result) {
    return "{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"result\":" + result + "}";
}

std::string LSPServer::createError(const std::string& id, int code, const std::string& message) {
    return "{\"jsonrpc\":\"2.0\",\"id\":" + id +
           ",\"error\":{\"code\":" + std::to_string(code) +
           ",\"message\":\"" + message + "\"}}";
}

void LSPServer::sendNotification(const std::string& method, const std::string& params) {
    std::string msg = "{\"jsonrpc\":\"2.0\",\"method\":\"" + method + "\",\"params\":" + params + "}";
    sendMessage(msg);
}

void LSPServer::publishDiagnostics(const std::string& uri, const std::vector<Diagnostic>& diagnostics) {
    std::ostringstream oss;
    oss << "{\"uri\":\"" << uri << "\",\"diagnostics\":[";
    for (size_t i = 0; i < diagnostics.size(); i++) {
        if (i > 0) oss << ",";
        const auto& d = diagnostics[i];
        oss << "{\"range\":{\"start\":{\"line\":" << d.range.start.line
            << ",\"character\":" << d.range.start.character
            << "},\"end\":{\"line\":" << d.range.end.line
            << ",\"character\":" << d.range.end.character
            << "}},\"severity\":" << d.severity
            << ",\"message\":\"" << d.message
            << "\",\"source\":\"" << d.source << "\"}";
    }
    oss << "]}";
    sendNotification("textDocument/publishDiagnostics", oss.str());
}

// ─── LSP methods ─────────────────────────────────────────────────────────

std::string LSPServer::handleInitialize(const std::string& id, const std::string&) {
    return createResponse(id, "{"
        "\"capabilities\":{"
            "\"textDocumentSync\":{\"openClose\":true,\"change\":1},"
            "\"completionProvider\":{\"triggerCharacters\":[\".\",\":\"]},"
            "\"definitionProvider\":true,"
            "\"hoverProvider\":true"
        "},"
        "\"serverInfo\":{\"name\":\"suki-lsp\",\"version\":\"0.1.0\"}"
    "}");
}

std::string LSPServer::handleInitialized(const std::string&, const std::string&) {
    return "";
}

std::string LSPServer::handleShutdown(const std::string& id, const std::string&) {
    return createResponse(id, "null");
}

std::string LSPServer::handleDidOpen(const std::string& params) {
    // Extract URI and text from params
    // Simple extraction for now
    Document doc;
    // TODO: proper JSON parsing
    documents_[doc.uri] = doc;
    analyzeDocument(doc.uri);
    return "";
}

std::string LSPServer::handleDidChange(const std::string& params) {
    // Update document content
    // TODO: proper JSON parsing and incremental updates
    return "";
}

std::string LSPServer::handleDidClose(const std::string& params) {
    // Remove document
    // TODO: proper JSON parsing
    return "";
}

std::string LSPServer::handleCompletion(const std::string& id, const std::string& params) {
    // TODO: proper completion based on context
    std::vector<CompletionItem> items;

    // Add keywords
    std::vector<std::string> keywords = {
        "func", "let", "var", "if", "else", "for", "in", "while",
        "return", "struct", "class", "enum", "protocol", "actor",
        "import", "module", "async", "await", "throws", "try",
        "true", "false", "nil", "self", "super"
    };

    for (const auto& kw : keywords) {
        CompletionItem item;
        item.label = kw;
        item.kind = 14; // Keyword
        items.push_back(item);
    }

    // Build response
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < items.size(); i++) {
        if (i > 0) oss << ",";
        oss << "{\"label\":\"" << items[i].label
            << "\",\"kind\":" << items[i].kind << "}";
    }
    oss << "]";

    return createResponse(id, oss.str());
}

std::string LSPServer::handleDefinition(const std::string& id, const std::string&) {
    // TODO: implement go-to-definition
    return createResponse(id, "null");
}

std::string LSPServer::handleHover(const std::string& id, const std::string&) {
    // TODO: implement hover information
    return createResponse(id, "null");
}

// ─── Analysis ────────────────────────────────────────────────────────────

void LSPServer::analyzeDocument(const std::string& uri) {
    auto it = documents_.find(uri);
    if (it == documents_.end()) return;

    auto diagnostics = diagnose(it->second.content, uri);
    publishDiagnostics(uri, diagnostics);
}

std::vector<Diagnostic> LSPServer::diagnose(const std::string& content, const std::string& uri) {
    std::vector<Diagnostic> diags;

    // Lex and parse
    DiagnosticEngine diag;
    Lexer lexer(content, uri, diag);
    auto tokens = lexer.lexAll();
    Parser parser(std::move(tokens), content, uri, diag);
    auto ast = parser.parse();

    // Convert compiler diagnostics to LSP diagnostics
    for (const auto& d : diag.diagnostics()) {
        Diagnostic ld;
        ld.range.start.line = d.loc.line - 1;
        ld.range.start.character = d.loc.column - 1;
        ld.range.end.line = d.loc.line - 1;
        ld.range.end.character = d.loc.column;
        ld.message = d.message;
        ld.severity = (d.level == DiagnosticLevel::Error) ? 1 :
                      (d.level == DiagnosticLevel::Warning) ? 2 : 3;
        diags.push_back(ld);
    }

    return diags;
}

std::vector<CompletionItem> LSPServer::getCompletions(const std::string&, Position) {
    return {};
}

// ─── IO ──────────────────────────────────────────────────────────────────

std::string LSPServer::readMessage() {
    // Read Content-Length header
    std::string line;
    int contentLength = 0;

    while (std::getline(std::cin, line)) {
        if (line.empty() || line == "\r") break;
        if (line.find("Content-Length:") == 0) {
            contentLength = std::stoi(line.substr(15));
        }
    }

    if (contentLength <= 0) return "";

    // Read content
    std::string content(contentLength, '\0');
    std::cin.read(&content[0], contentLength);
    return content;
}

void LSPServer::sendMessage(const std::string& message) {
    std::cout << "Content-Length: " << message.size() << "\r\n\r\n" << message;
    std::cout.flush();
}

} // namespace suki::lsp
