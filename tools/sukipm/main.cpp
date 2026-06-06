// sukipm — SukiCode 包管理器
// Package manager for SukiCode.
//
// Usage:
//   sukipm init <name>         — create new package
//   sukipm build               — build project
//   sukipm test                — run tests
//   sukipm add <package>       — add dependency
//   sukipm remove <package>    — remove dependency
//   sukipm update              — update dependencies
//   sukipm publish             — publish to registry
//   sukipm docs                — generate documentation
//   sukipm clean               — clean build artifacts

#include "Manifest.h"
#include "DependencyResolver.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <filesystem>

namespace fs = std::filesystem;

static void printHelp() {
    std::cout << R"(SukiCode Package Manager (sukipm)

Usage:
  sukipm <command> [options]

Commands:
  init <name>       Create new package
  build             Build project
  test              Run tests
  add <package>     Add dependency
  remove <package>  Remove dependency
  update            Update dependencies
  publish           Publish to registry
  docs              Generate documentation
  clean             Clean build artifacts

Options:
  -v, --verbose     Verbose output
  -h, --help        Show this help
)";
}

// 查找清单文件 / Find manifest file
static std::string findManifest() {
    for (const auto& entry : fs::directory_iterator(".")) {
        if (entry.path().extension() == ".sukiproj") {
            return entry.path().string();
        }
    }
    return "";
}

// 初始化新包 / Initialize new package
static int initPackage(const std::string& name) {
    std::string manifestPath = name + ".sukiproj";
    if (fs::exists(manifestPath)) {
        std::cerr << "sukipm: package already exists: " << manifestPath << "\n";
        return 1;
    }

    suki::pm::Manifest manifest;
    manifest.name = name;
    manifest.version = "0.1.0";
    manifest.description = "";
    manifest.license = "MIT";

    // Create directory structure
    fs::create_directories("Sources/" + name);
    fs::create_directories("Tests/" + name + "Tests");

    // Create manifest file
    std::ofstream file(manifestPath);
    file << manifest.dump();

    // Create main source file
    std::ofstream srcFile("Sources/" + name + "/main.suki");
    srcFile << "// " << name << " - SukiCode package\n\n";
    srcFile << "@main\n";
    srcFile << "func main() -> Int {\n";
    srcFile << "    print(\"Hello from " << name << "!\")\n";
    srcFile << "    return 0\n";
    srcFile << "}\n";

    std::cout << "sukipm: created package '" << name << "'\n";
    std::cout << "  " << manifestPath << "\n";
    std::cout << "  Sources/" << name << "/main.suki\n";
    std::cout << "  Tests/" << name << "Tests/\n";
    return 0;
}

// 构建项目 / Build project
static int buildProject(bool verbose) {
    std::string manifestPath = findManifest();
    if (manifestPath.empty()) {
        std::cerr << "sukipm: no .sukiproj file found\n";
        return 1;
    }

    // Read manifest
    std::ifstream file(manifestPath);
    std::ostringstream ss;
    ss << file.rdbuf();
    auto manifest = suki::pm::Manifest::parse(ss.str());

    if (verbose) {
        std::cout << "sukipm: building " << manifest.name << " v" << manifest.version << "\n";
    }

    // Find source files
    std::string sourcesDir = "Sources/" + manifest.name;
    if (!fs::exists(sourcesDir)) {
        sourcesDir = "Sources";
    }

    // Compile each source file
    int result = 0;
    for (const auto& entry : fs::recursive_directory_iterator(sourcesDir)) {
        if (entry.path().extension() == ".suki") {
            std::string cmd = "sukic -o " + entry.path().stem().string() + ".exe " + entry.path().string();
            if (verbose) std::cout << "  " << cmd << "\n";
            int r = system(cmd.c_str());
            if (r != 0) result = r;
        }
    }

    if (result == 0) {
        std::cout << "sukipm: build successful\n";
    } else {
        std::cerr << "sukipm: build failed\n";
    }
    return result;
}

// 运行测试 / Run tests
static int runTests(bool verbose) {
    std::string testDir = "Tests";
    if (!fs::exists(testDir)) {
        std::cerr << "sukipm: no Tests directory found\n";
        return 1;
    }

    int result = 0;
    for (const auto& entry : fs::recursive_directory_iterator(testDir)) {
        if (entry.path().extension() == ".suki" &&
            entry.path().stem().string().find("Test") != std::string::npos) {
            std::string cmd = "sukic -o /tmp/test_out.exe " + entry.path().string();
            if (verbose) std::cout << "  " << cmd << "\n";
            int r = system(cmd.c_str());
            if (r == 0) {
                r = system("/tmp/test_out.exe");
            }
            if (r != 0) result = r;
        }
    }

    return result;
}

// 添加依赖 / Add dependency
static int addDependency(const std::string& name) {
    std::string manifestPath = findManifest();
    if (manifestPath.empty()) {
        std::cerr << "sukipm: no .sukiproj file found\n";
        return 1;
    }

    std::cout << "sukipm: adding dependency '" << name << "'\n";
    // TODO: Parse manifest, add dependency, rewrite file
    std::cout << "sukipm: dependency added (manifest update not yet implemented)\n";
    return 0;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printHelp();
        return 1;
    }

    std::string command = argv[1];
    bool verbose = false;

    // Handle help flags
    if (command == "-h" || command == "--help") {
        printHelp();
        return 0;
    }

    for (int i = 2; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "-v" || arg == "--verbose") verbose = true;
        else if (arg == "-h" || arg == "--help") { printHelp(); return 0; }
    }

    if (command == "init") {
        if (argc < 3) {
            std::cerr << "sukipm: usage: sukipm init <name>\n";
            return 1;
        }
        return initPackage(argv[2]);
    } else if (command == "build") {
        return buildProject(verbose);
    } else if (command == "test") {
        return runTests(verbose);
    } else if (command == "add") {
        if (argc < 3) {
            std::cerr << "sukipm: usage: sukipm add <package>\n";
            return 1;
        }
        return addDependency(argv[2]);
    } else if (command == "clean") {
        fs::remove_all(".build");
        std::cout << "sukipm: cleaned\n";
        return 0;
    } else if (command == "publish") {
        std::cerr << "sukipm: publish not yet implemented\n";
        return 1;
    } else if (command == "docs") {
        std::cerr << "sukipm: docs not yet implemented\n";
        return 1;
    } else {
        std::cerr << "sukipm: unknown command: " << command << "\n";
        printHelp();
        return 1;
    }
}
