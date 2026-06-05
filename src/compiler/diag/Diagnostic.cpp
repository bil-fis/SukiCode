// Diagnostic engine implementation.

#include "Diagnostic.h"
#include <iostream>
#include <sstream>

namespace suki {

DiagnosticEngine::DiagnosticEngine() = default;

void DiagnosticEngine::error(SourceLocation loc, std::string_view filename, std::string_view message) {
    report(DiagnosticLevel::Error, loc, filename, message);
}

void DiagnosticEngine::warning(SourceLocation loc, std::string_view filename, std::string_view message) {
    report(DiagnosticLevel::Warning, loc, filename, message);
}

void DiagnosticEngine::note(SourceLocation loc, std::string_view filename, std::string_view message) {
    report(DiagnosticLevel::Note, loc, filename, message);
}

void DiagnosticEngine::report(DiagnosticLevel level, SourceLocation loc,
                               std::string_view filename, std::string_view message) {
    Diagnostic diag;
    diag.level = level;
    diag.loc = loc;
    diag.filename = filename;
    diag.message = std::string(message);

    diagnostics_.push_back(diag);

    if (level == DiagnosticLevel::Error) errorCount_++;
    if (level == DiagnosticLevel::Warning) warningCount_++;

    if (handler_) {
        handler_(diag);
    }
}

void DiagnosticEngine::printAll(std::string_view source, std::string_view filename) const {
    for (const auto& diag : diagnostics_) {
        const char* prefix = "";
        switch (diag.level) {
            case DiagnosticLevel::Error:   prefix = "error: "; break;
            case DiagnosticLevel::Warning: prefix = "warning: "; break;
            case DiagnosticLevel::Note:    prefix = "note: "; break;
        }

        std::cerr << diag.filename << ":" << diag.loc.line << ":" << diag.loc.column
                  << ": " << prefix << diag.message << "\n";

        // Show the source line
        if (diag.loc.offset < source.size()) {
            // Find line start
            uint32_t lineStart = diag.loc.offset;
            while (lineStart > 0 && source[lineStart - 1] != '\n') lineStart--;

            // Find line end
            uint32_t lineEnd = diag.loc.offset;
            while (lineEnd < source.size() && source[lineEnd] != '\n') lineEnd++;

            std::string lineStr(source.substr(lineStart, lineEnd - lineStart));
            std::cerr << lineStr << "\n";

            // Caret pointing to the error
            uint32_t caretPos = diag.loc.column - 1;
            std::string caret(caretPos, ' ');
            caret += '^';
            std::cerr << caret << "\n";
        }
    }
}

} // namespace suki
