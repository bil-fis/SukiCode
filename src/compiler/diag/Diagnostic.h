#pragma once
// Diagnostic engine for the SukiCode compiler.
// Collects errors, warnings, and notes during compilation.

#include "compiler/lexer/Token.h"
#include <string>
#include <string_view>
#include <vector>
#include <functional>

namespace suki {

enum class DiagnosticLevel : uint8_t {
    Error,
    Warning,
    Note,
};

struct Diagnostic {
    DiagnosticLevel level;
    SourceLocation loc;
    std::string_view filename;
    std::string message;
};

class DiagnosticEngine {
public:
    using Handler = std::function<void(const Diagnostic&)>;

    DiagnosticEngine();

    // Report diagnostics
    void error(SourceLocation loc, std::string_view filename, std::string_view message);
    void warning(SourceLocation loc, std::string_view filename, std::string_view message);
    void note(SourceLocation loc, std::string_view filename, std::string_view message);

    // Error count
    bool hadErrors() const { return errorCount_ > 0; }
    int errorCount() const { return errorCount_; }
    int warningCount() const { return warningCount_; }

    // Access diagnostics
    const std::vector<Diagnostic>& diagnostics() const { return diagnostics_; }

    // Set custom handler (e.g., for LSP)
    void setHandler(Handler handler) { handler_ = std::move(handler); }

    // Print all diagnostics to stderr
    void printAll(std::string_view source, std::string_view filename) const;

private:
    void report(DiagnosticLevel level, SourceLocation loc, std::string_view filename, std::string_view message);

    std::vector<Diagnostic> diagnostics_;
    Handler handler_;
    int errorCount_ = 0;
    int warningCount_ = 0;
};

} // namespace suki
