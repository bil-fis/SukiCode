#include "compiler/diag/DiagnosticEngine.h"

#include <cstdio>

namespace suki {

bool DiagnosticEngine::emit(FILE* out, bool printSummary) {
    for (const Diagnostic& d : diagnostics_) {
        const SourceLocation& p = d.range().start;
        if (p.isValid()) {
            std::fprintf(out, "%s:%u:%u: %s: %s\n",
                         "(source)", p.line, p.column,
                         d.severityName(), d.message().c_str());
        } else {
            std::fprintf(out, "error: %s\n", d.message().c_str());
        }
    }

    if (printSummary && !diagnostics_.empty()) {
        if (errorCount_ > 0) {
            std::fprintf(out, "%u error(s)", errorCount_);
            if (warningCount_ > 0) std::fprintf(out, ", %u warning(s)", warningCount_);
            std::fprintf(out, " generated.\n");
        } else if (warningCount_ > 0) {
            std::fprintf(out, "%u warning(s) generated.\n", warningCount_);
        }
    }

    return !hasErrors();
}

} // namespace suki
