// SukiLSP implementation - Language Server Protocol server.

#include "LSPServer.h"
#include "JsonParser.h"
#include "compiler/lexer/Lexer.h"
#include "compiler/parser/Parser.h"
#include "compiler/diag/Diagnostic.h"

#include <iostream>
#include <sstream>

namespace suki::lsp {

LSPServer::LSPServer() = default;
LSPServer::~LSPServer() = default;

void LSPServer::run() {
    while (running_) {
        std::string raw = readMessage();
        if (raw.empty()) continue;

        auto json = JsonParser::parse(raw);
        if (!json.isObject()) continue;

        Message msg;
        const auto& methodVal = json["method"];
        if (methodVal.isString()) msg.method = methodVal.stringValue();

        const auto& idVal = json["id"];
        if (idVal.isNumber()) msg.id = std::to_string(idVal.intValue());
        else if (idVal.isString()) msg.id = idVal.stringValue();

        msg.params = json.hasKey("params") ? json["params"].dump() : "";

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
    auto json = JsonParser::parse(params);
    const auto& td = json["textDocument"];
    if (!td.isObject()) return "";

    Document doc;
    if (td["uri"].isString()) doc.uri = td["uri"].stringValue();
    if (td["text"].isString()) doc.content = td["text"].stringValue();
    if (td["version"].isNumber()) doc.version = td["version"].intValue();

    documents_[doc.uri] = doc;
    analyzeDocument(doc.uri);
    return "";
}

std::string LSPServer::handleDidChange(const std::string& params) {
    auto json = JsonParser::parse(params);
    const auto& td = json["textDocument"];
    const auto& changes = json["contentChanges"];
    if (!td.isObject() || !changes.isArray()) return "";

    std::string uri;
    if (td["uri"].isString()) uri = td["uri"].stringValue();

    auto it = documents_.find(uri);
    if (it == documents_.end()) return "";

    // Apply changes (full document sync)
    if (changes.size() > 0) {
        const auto& change = changes[0];
        if (change.hasKey("text") && change["text"].isString()) {
            it->second.content = change["text"].stringValue();
        }
    }

    if (td["version"].isNumber()) {
        it->second.version = td["version"].intValue();
    }

    analyzeDocument(uri);
    return "";
}

std::string LSPServer::handleDidClose(const std::string& params) {
    auto json = JsonParser::parse(params);
    const auto& td = json["textDocument"];
    if (!td.isObject()) return "";

    std::string uri;
    if (td["uri"].isString()) uri = td["uri"].stringValue();
    documents_.erase(uri);
    return "";
}

std::string LSPServer::handleCompletion(const std::string& id, const std::string& params) {
    auto json = JsonParser::parse(params);
    const auto& td = json["textDocument"];
    const auto& pos = json["position"];

    std::string uri;
    if (td["uri"].isString()) uri = td["uri"].stringValue();

    Position position;
    if (pos["line"].isNumber()) position.line = pos["line"].intValue();
    if (pos["character"].isNumber()) position.character = pos["character"].intValue();

    auto items = getCompletions(uri, position);

    std::ostringstream oss;
    oss << "{\"isIncomplete\":false,\"items\":[";
    for (size_t i = 0; i < items.size(); i++) {
        if (i > 0) oss << ",";
        oss << "{\"label\":\"" << items[i].label
            << "\",\"kind\":" << items[i].kind;
        if (!items[i].detail.empty()) {
            oss << ",\"detail\":\"" << items[i].detail << "\"";
        }
        oss << "}";
    }
    oss << "]}";

    return createResponse(id, oss.str());
}

std::string LSPServer::handleDefinition(const std::string& id, const std::string& params) {
    auto json = JsonParser::parse(params);
    const auto& td = json["textDocument"];
    const auto& pos = json["position"];

    std::string uri;
    if (td["uri"].isString()) uri = td["uri"].stringValue();

    Position position;
    if (pos["line"].isNumber()) position.line = pos["line"].intValue();
    if (pos["character"].isNumber()) position.character = pos["character"].intValue();

    // TODO: implement actual go-to-definition
    // For now, return the same position (no-op)
    return createResponse(id, "null");
}

std::string LSPServer::handleHover(const std::string& id, const std::string& params) {
    auto json = JsonParser::parse(params);
    const auto& td = json["textDocument"];
    const auto& pos = json["position"];

    std::string uri;
    if (td["uri"].isString()) uri = td["uri"].stringValue();

    Position position;
    if (pos["line"].isNumber()) position.line = pos["line"].intValue();
    if (pos["character"].isNumber()) position.character = pos["character"].intValue();

    // TODO: implement actual hover information
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

    DiagnosticEngine diag;
    Lexer lexer(content, uri, diag);
    auto tokens = lexer.lexAll();
    Parser parser(std::move(tokens), content, uri, diag);
    auto ast = parser.parse();

    for (const auto& d : diag.diagnostics()) {
        Diagnostic ld;
        ld.range.start.line = d.loc.line > 0 ? d.loc.line - 1 : 0;
        ld.range.start.character = d.loc.column > 0 ? d.loc.column - 1 : 0;
        ld.range.end.line = ld.range.start.line;
        ld.range.end.character = ld.range.start.character + 1;
        ld.message = d.message;
        ld.severity = (d.level == DiagnosticLevel::Error) ? 1 :
                      (d.level == DiagnosticLevel::Warning) ? 2 : 3;
        diags.push_back(ld);
    }

    return diags;
}

std::vector<CompletionItem> LSPServer::getCompletions(const std::string&, Position) {
    std::vector<CompletionItem> items;

    // Keywords
    std::vector<std::pair<std::string, std::string>> keywords = {
        {"func", "Function declaration"},
        {"let", "Constant declaration"},
        {"var", "Variable declaration"},
        {"if", "If statement"},
        {"else", "Else clause"},
        {"for", "For loop"},
        {"in", "In keyword"},
        {"while", "While loop"},
        {"return", "Return statement"},
        {"struct", "Struct declaration"},
        {"class", "Class declaration"},
        {"enum", "Enum declaration"},
        {"protocol", "Protocol declaration"},
        {"actor", "Actor declaration"},
        {"import", "Import declaration"},
        {"module", "Module declaration"},
        {"async", "Async function"},
        {"await", "Await expression"},
        {"throws", "Throwing function"},
        {"try", "Try expression"},
        {"true", "Boolean true"},
        {"false", "Boolean false"},
        {"nil", "Nil literal"},
        {"self", "Self reference"},
        {"super", "Super reference"},
        {"guard", "Guard statement"},
        {"switch", "Switch statement"},
        {"case", "Case label"},
        {"default", "Default case"},
        {"break", "Break statement"},
        {"continue", "Continue statement"},
        {"fallthrough", "Fallthrough"},
        {"defer", "Defer statement"},
        {"select", "Select statement"},
        {"unsafe", "Unsafe block"},
        {"extension", "Extension declaration"},
        {"typealias", "Type alias"},
        {"init", "Initializer"},
        {"deinit", "Deinitializer"},
        {"subscript", "Subscript"},
        {"static", "Static member"},
        {"mutating", "Mutating method"},
        {"override", "Override"},
        {"final", "Final"},
        {"required", "Required"},
        {"convenience", "Convenience init"},
    };

    for (const auto& [kw, doc] : keywords) {
        CompletionItem item;
        item.label = kw;
        item.kind = 14; // Keyword
        item.detail = doc;
        items.push_back(item);
    }

    // Types
    std::vector<std::string> types = {
        "Int", "Int8", "Int16", "Int32", "Int64",
        "UInt", "UInt8", "UInt16", "UInt32", "UInt64",
        "Float", "Double", "Bool", "String", "Char", "Void",
        "Array", "Dictionary", "Set", "Optional", "Result",
        "Any", "AnyObject", "Self",
    };

    for (const auto& ty : types) {
        CompletionItem item;
        item.label = ty;
        item.kind = 7; // Type
        items.push_back(item);
    }

    return items;
}

// ─── IO ──────────────────────────────────────────────────────────────────

std::string LSPServer::readMessage() {
    std::string line;
    int contentLength = 0;

    while (std::getline(std::cin, line)) {
        if (line.empty() || line == "\r") break;
        if (line.find("Content-Length:") == 0) {
            contentLength = std::stoi(line.substr(15));
        }
    }

    if (contentLength <= 0) return "";

    std::string content(contentLength, '\0');
    std::cin.read(&content[0], contentLength);
    return content;
}

void LSPServer::sendMessage(const std::string& message) {
    std::cout << "Content-Length: " << message.size() << "\r\n\r\n" << message;
    std::cout.flush();
}

} // namespace suki::lsp
