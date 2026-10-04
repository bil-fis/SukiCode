// suki-fmt — SukiCode 代码格式化工具
// Code formatter for SukiCode source files.
//
// Usage:
//   suki-fmt [options] <file.suki>
//   suki-fmt --check <file.suki>   — check if file is formatted
//
// Options:
//   --indent <n>      — indentation width (default: 4)
//   --line-length <n> — max line length (default: 100)
//   --use-tabs        — use tabs instead of spaces
//   --no-trailing     — no trailing commas
//   --check           — check mode (exit 1 if not formatted)
//   -h, --help        — show help

#include "Formatter.h"
#include "compiler/lexer/Lexer.h"
#include "compiler/parser/Parser.h"
#include "compiler/diag/Diagnostic.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>

static void printHelp() {
    std::cout << R"(SukiCode Formatter (suki-fmt)

Usage:
  suki-fmt [options] <file.suki>

Options:
  --indent <n>      Indentation width (default: 4)
  --line-length <n> Max line length (default: 100)
  --use-tabs        Use tabs instead of spaces
  --no-trailing     No trailing commas
  --check           Check mode (exit 1 if not formatted)
  -h, --help        Show this help
)";
}

int main(int argc, char* argv[]) {
    std::string inputFile;
    suki::FormatConfig config;
    bool checkMode = false;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            printHelp();
            return 0;
        } else if (arg == "--check") {
            checkMode = true;
        } else if (arg == "--indent" && i + 1 < argc) {
            config.indentWidth = std::stoi(argv[++i]);
        } else if (arg == "--line-length" && i + 1 < argc) {
            config.lineLength = std::stoi(argv[++i]);
        } else if (arg == "--use-tabs") {
            config.useSpaces = false;
        } else if (arg == "--no-trailing") {
            config.trailingCommas = false;
        } else if (arg[0] != '-') {
            inputFile = arg;
        }
    }

    if (inputFile.empty()) {
        std::cerr << "suki-fmt: no input file\n";
        printHelp();
        return 1;
    }

    // 读取源文件 / Read source file
    std::ifstream file(inputFile, std::ios::binary);
    if (!file) {
        std::cerr << "suki-fmt: cannot open file: " << inputFile << "\n";
        return 1;
    }
    std::ostringstream ss;
    ss << file.rdbuf();
    std::string source = ss.str();

    // 解析 / Parse
    suki::DiagnosticEngine diag;
    suki::Lexer lexer(source, inputFile, diag);
    auto tokens = lexer.lexAll();
    suki::Parser parser(std::move(tokens), source, inputFile, diag);
    auto ast = parser.parse();

    if (diag.hadErrors()) {
        diag.printAll(source, inputFile);
        return 1;
    }

    // 格式化 / Format
    suki::Formatter formatter(config);
    std::string formatted = formatter.format(*ast);

    if (checkMode) {
        // 检查模式：比较格式化结果 / Check mode: compare
        if (formatted != source) {
            std::cerr << "suki-fmt: " << inputFile << " is not formatted\n";
            return 1;
        }
        return 0;
    }

    // 输出格式化结果 / Output formatted result
    std::cout << formatted;
    return 0;
}
