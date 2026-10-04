#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace suki {

// ─── Source location ──────────────────────────────────────────────────────
// Lightweight source position. A full SourceManager (for multi-file,
// source-line rendering) is added when diagnostics need caret rendering.
struct SourceLocation {
    uint32_t line   = 1;
    uint32_t column = 1;
    uint32_t offset = 0; // byte offset from start of source

    bool isValid() const { return offset != 0u; }
    bool operator==(const SourceLocation& o) const {
        return line == o.line && column == o.column && offset == o.offset;
    }
};

struct SourceRange {
    SourceLocation start;
    SourceLocation end;

    bool isValid() const { return start.isValid() || end.isValid(); }
};

// ─── Severity ─────────────────────────────────────────────────────────────
enum class DiagnosticSeverity {
    Note,
    Warning,
    Error,
    Fatal,
};

// ─── Diagnostic ─────────────────────────────────────────────────────────────
// A single diagnostic message bound to an optional source range.
// Designed to be cheap to construct and carry by value.
class Diagnostic {
public:
    Diagnostic(DiagnosticSeverity severity, std::string message, SourceRange range = {})
        : severity_(severity), message_(std::move(message)), range_(range) {}

    DiagnosticSeverity severity() const { return severity_; }
    const std::string& message() const { return message_; }
    const SourceRange& range() const { return range_; }

    bool isError() const {
        return severity_ == DiagnosticSeverity::Error ||
               severity_ == DiagnosticSeverity::Fatal;
    }

    const char* severityName() const {
        switch (severity_) {
            case DiagnosticSeverity::Note:    return "note";
            case DiagnosticSeverity::Warning: return "warning";
            case DiagnosticSeverity::Error:   return "error";
            case DiagnosticSeverity::Fatal:   return "fatal error";
        }
        return "error";
    }

private:
    DiagnosticSeverity severity_;
    std::string message_;
    SourceRange range_;
};

} // namespace suki
