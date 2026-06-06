// suki-doc — SukiCode 文档生成器
// Documentation generator from /// comments.
//
// Usage:
//   suki-doc [options] <source-dir>
//
// Options:
//   --output <dir>    Output directory (default: docs/)
//   --format <fmt>    Output format: html, markdown (default: html)
//   -h, --help        Show this help

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <filesystem>
#include <regex>

namespace fs = std::filesystem;

static void printHelp() {
    std::cout << R"(SukiCode Documentation Generator (suki-doc)

Usage:
  suki-doc [options] <source-dir>

Options:
  --output <dir>    Output directory (default: docs/)
  --format <fmt>    Output format: html, markdown (default: html)
  -h, --help        Show this help
)";
}

// 提取文档注释 / Extract doc comments
struct DocEntry {
    std::string name;
    std::string documentation;
    std::string filePath;
    int line;
};

// 从源文件提取文档注释 / Extract doc comments from source file
std::vector<DocEntry> extractDocs(const std::string& filePath) {
    std::vector<DocEntry> entries;
    std::ifstream file(filePath);
    if (!file) return entries;

    std::string line;
    int lineNum = 0;
    bool inDocComment = false;
    std::string currentDoc;

    while (std::getline(file, line)) {
        lineNum++;

        // Trim
        std::string trimmed = line;
        trimmed.erase(0, trimmed.find_first_not_of(" \t"));

        if (trimmed.substr(0, 3) == "///") {
            // Doc comment line
            std::string content = trimmed.substr(3);
            if (!content.empty() && content[0] == ' ') content = content.substr(1);
            currentDoc += content + "\n";
            inDocComment = true;
        } else if (inDocComment) {
            // End of doc comment block - look for declaration
            if (!trimmed.empty() && trimmed[0] != '/' && trimmed[0] != '*') {
                // Try to extract name from declaration
                std::string name;
                if (trimmed.find("func ") == 0) {
                    size_t nameStart = 5;
                    size_t nameEnd = trimmed.find_first_of("(:< \t", nameStart);
                    if (nameEnd == std::string::npos) nameEnd = trimmed.size();
                    name = trimmed.substr(nameStart, nameEnd - nameStart);
                } else if (trimmed.find("struct ") == 0) {
                    size_t nameStart = 7;
                    size_t nameEnd = trimmed.find_first_of(" {:<\t", nameStart);
                    if (nameEnd == std::string::npos) nameEnd = trimmed.size();
                    name = trimmed.substr(nameStart, nameEnd - nameStart);
                } else if (trimmed.find("class ") == 0) {
                    size_t nameStart = 6;
                    size_t nameEnd = trimmed.find_first_of(" {:<\t", nameStart);
                    if (nameEnd == std::string::npos) nameEnd = trimmed.size();
                    name = trimmed.substr(nameStart, nameEnd - nameStart);
                } else if (trimmed.find("let ") == 0 || trimmed.find("var ") == 0) {
                    bool isLet = trimmed[0] == 'l';
                    size_t nameStart = isLet ? 4 : 4;
                    size_t nameEnd = trimmed.find_first_of(" :=\t", nameStart);
                    if (nameEnd == std::string::npos) nameEnd = trimmed.size();
                    name = trimmed.substr(nameStart, nameEnd - nameStart);
                }

                if (!name.empty() && !currentDoc.empty()) {
                    DocEntry entry;
                    entry.name = name;
                    entry.documentation = currentDoc;
                    entry.filePath = filePath;
                    entry.line = lineNum - 1;
                    entries.push_back(entry);
                }
                currentDoc.clear();
                inDocComment = false;
            }
        }
    }

    return entries;
}

// 生成 Markdown 文档 / Generate Markdown documentation
std::string generateMarkdown(const std::vector<DocEntry>& entries, const std::string& title) {
    std::ostringstream oss;
    oss << "# " << title << "\n\n";

    for (const auto& entry : entries) {
        oss << "## " << entry.name << "\n\n";
        oss << entry.documentation << "\n";
        oss << "*Source: " << entry.filePath << ":" << entry.line << "*\n\n";
    }

    return oss.str();
}

// 生成 HTML 文档 / Generate HTML documentation
std::string generateHTML(const std::vector<DocEntry>& entries, const std::string& title) {
    std::ostringstream oss;
    oss << "<!DOCTYPE html>\n<html>\n<head>\n";
    oss << "<title>" << title << "</title>\n";
    oss << "<style>body{font-family:sans-serif;max-width:800px;margin:0 auto;padding:20px}\n";
    oss << "h1{color:#333}h2{color:#666;border-bottom:1px solid #eee;padding-bottom:5px}\n";
    oss << "pre{background:#f5f5f5;padding:10px;border-radius:4px}</style>\n";
    oss << "</head>\n<body>\n";
    oss << "<h1>" << title << "</h1>\n";

    for (const auto& entry : entries) {
        oss << "<h2>" << entry.name << "</h2>\n";
        oss << "<p>" << entry.documentation << "</p>\n";
        oss << "<p><em>Source: " << entry.filePath << ":" << entry.line << "</em></p>\n";
    }

    oss << "</body>\n</html>\n";
    return oss.str();
}

int main(int argc, char* argv[]) {
    std::string sourceDir = ".";
    std::string outputDir = "docs";
    std::string format = "html";

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            printHelp();
            return 0;
        } else if (arg == "--output" && i + 1 < argc) {
            outputDir = argv[++i];
        } else if (arg == "--format" && i + 1 < argc) {
            format = argv[++i];
        } else if (arg[0] != '-') {
            sourceDir = arg;
        }
    }

    // Collect all .suki files
    std::vector<DocEntry> allDocs;
    for (const auto& entry : fs::recursive_directory_iterator(sourceDir)) {
        if (entry.path().extension() == ".suki") {
            auto docs = extractDocs(entry.path().string());
            allDocs.insert(allDocs.end(), docs.begin(), docs.end());
        }
    }

    if (allDocs.empty()) {
        std::cout << "suki-doc: no documentation comments found\n";
        return 0;
    }

    // Generate output
    fs::create_directories(outputDir);
    std::string title = "SukiCode Documentation";

    if (format == "markdown" || format == "md") {
        std::string content = generateMarkdown(allDocs, title);
        std::ofstream file(outputDir + "/index.md");
        file << content;
        std::cout << "suki-doc: generated " << outputDir << "/index.md\n";
    } else {
        std::string content = generateHTML(allDocs, title);
        std::ofstream file(outputDir + "/index.html");
        file << content;
        std::cout << "suki-doc: generated " << outputDir << "/index.html\n";
    }

    return 0;
}
