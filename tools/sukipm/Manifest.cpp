// SukiPM manifest parser implementation.

#include "Manifest.h"
#include <sstream>
#include <algorithm>

namespace suki::pm {

Manifest Manifest::parse(const std::string& content) {
    Manifest m;

    // Simple line-based parser for .sukiproj format
    // TODO: Use proper SukiCode parser for full syntax support
    std::istringstream stream(content);
    std::string line;

    while (std::getline(stream, line)) {
        // Trim whitespace
        line.erase(0, line.find_first_not_of(" \t"));
        line.erase(line.find_last_not_of(" \t") + 1);

        // Skip comments and empty lines
        if (line.empty() || line[0] == '/' || line[0] == '#') continue;

        // Parse key: value pairs
        size_t colonPos = line.find(':');
        if (colonPos == std::string::npos) continue;

        std::string key = line.substr(0, colonPos);
        std::string value = line.substr(colonPos + 1);

        // Trim
        key.erase(0, key.find_first_not_of(" \t"));
        key.erase(key.find_last_not_of(" \t") + 1);
        value.erase(0, value.find_first_not_of(" \t"));
        value.erase(value.find_last_not_of(" \t") + 1);

        // Remove quotes
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
            value = value.substr(1, value.size() - 2);
        }

        if (key == "name") m.name = value;
        else if (key == "version") m.version = value;
        else if (key == "description") m.description = value;
        else if (key == "license") m.license = value;
        else if (key == "authors") {
            // Parse array: ["Alice <alice@example.com>"]
            // Simple: just store as single author
            m.authors.push_back(value);
        }
    }

    return m;
}

std::string Manifest::dump() const {
    std::ostringstream oss;
    oss << "package {\n";
    oss << "    name: \"" << name << "\"\n";
    oss << "    version: \"" << version << "\"\n";
    if (!description.empty()) {
        oss << "    description: \"" << description << "\"\n";
    }
    if (!authors.empty()) {
        oss << "    authors: [";
        for (size_t i = 0; i < authors.size(); i++) {
            if (i > 0) oss << ", ";
            oss << "\"" << authors[i] << "\"";
        }
        oss << "]\n";
    }
    if (!license.empty()) {
        oss << "    license: \"" << license << "\"\n";
    }
    oss << "}\n";
    return oss.str();
}

} // namespace suki::pm
