#pragma once

#include "compiler/diag/Diagnostic.h"
#include <cstdio>
#include <string>
#include <vector>

namespace suki {

// Collects diagnostics during compilation and reports them to stderr.
// Every compiler stage reports through a shared DiagnosticEngine so that
// error/warning counts and output formatting stay consistent.
class DiagnosticEngine {
public:
    void report(Diagnostic diag) {
        if (diag.isError()) {
            ++errorCount_;
        } else if (diag.severity() == DiagnosticSeverity::Warning) {
            ++warningCount_;
        }
        diagnostics_.push_back(std::move(diag));
    }

    void reportError(const std::string& msg, SourceRange range = {}) {
        report(Diagnostic(DiagnosticSeverity::Error, msg, range));
    }
    void reportFatal(const std::string& msg, SourceRange range = {}) {
        report(Diagnostic(DiagnosticSeverity::Fatal, msg, range));
    }
    void reportWarning(const std::string& msg, SourceRange range = {}) {
        report(Diagnostic(DiagnosticSeverity::Warning, msg, range));
    }
    void reportNote(const std::string& msg, SourceRange range = {}) {
        report(Diagnostic(DiagnosticSeverity::Note, msg, range));
    }

    bool hasErrors() const { return errorCount_ > 0; }
    uint32_t errorCount() const { return errorCount_; }
    uint32_t warningCount() const { return warningCount_; }
    size_t count() const { return diagnostics_.size(); }

    // Print all collected diagnostics to the given stream.
    // Returns false if any error/fatal was reported.
    bool emit(FILE* out = stderr, bool printSummary = true);

    void clear() {
        diagnostics_.clear();
        errorCount_ = 0;
        warningCount_ = 0;
    }

private:
    std::vector<Diagnostic> diagnostics_;
    uint32_t errorCount_   = 0;
    uint32_t warningCount_ = 0;
};

} // namespace suki
