cmake_minimum_required(VERSION 3.20)

# SukiCode Programming Language Toolchain
# A modern, memory-safe systems programming language with an LLVM backend.
project(SukiCode
    VERSION 0.1.0
    LANGUAGES C CXX
    DESCRIPTION "SukiCode programming language toolchain"
)

# ─── Global Settings ───────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# ─── Options ───────────────────────────────────────────────────────────────
# Tests/tools are built incrementally as the toolchain is implemented.
option(SUKICODE_BUILD_TESTS   "Build test suite"            OFF)
option(SUKICODE_BUILD_TOOLS   "Build tools (suki-lsp, etc)" OFF)
option(SUKICODE_BUILD_SUKIPM  "Build package manager"       OFF)
option(SUKICODE_USE_SYSTEM_LLVM "Use system LLVM installation" ON)

# ─── Find LLVM ─────────────────────────────────────────────────────────────
# LLVM is the sole cross-platform code-generation backend.
if(SUKICODE_USE_SYSTEM_LLVM)
    # Drive everything from llvm-config. It supplies the include/lib paths and
    # the exact RTTI setting without importing LLVM's exported CMake targets.
    # Importing those targets aborts configuration on distro LLVM packages
    # whose *static* component set is incomplete (Ubuntu, for example, omits
    # several clangBasic/LLVM archives) even though the shared libLLVM is
    # perfectly usable. We therefore link the monolithic LLVM from --libs.
    find_program(LLVM_CONFIG_EXECUTABLE
        NAMES llvm-config llvm-config-20 llvm-config-19 llvm-config-18
              llvm-config-17 llvm-config-16 llvm-config-15
        PATHS
            /usr/bin
            /usr/local/bin
            /opt/homebrew/bin
            "C:/Program Files/LLVM/bin"
            "$ENV{LLVM_DIR}/bin"
    )
    if(LLVM_CONFIG_EXECUTABLE)
        execute_process(COMMAND ${LLVM_CONFIG_EXECUTABLE} --version  OUTPUT_VARIABLE LLVM_VERSION_STRING OUTPUT_STRIP_TRAILING_WHITESPACE)
        execute_process(COMMAND ${LLVM_CONFIG_EXECUTABLE} --libdir   OUTPUT_VARIABLE LLVM_LIBDIR         OUTPUT_STRIP_TRAILING_WHITESPACE)
        execute_process(COMMAND ${LLVM_CONFIG_EXECUTABLE} --includedir OUTPUT_VARIABLE LLVM_INCLUDEDIR    OUTPUT_STRIP_TRAILING_WHITESPACE)
        execute_process(COMMAND ${LLVM_CONFIG_EXECUTABLE} --system-libs --libs all OUTPUT_VARIABLE LLVM_LIBS OUTPUT_STRIP_TRAILING_WHITESPACE)
        execute_process(COMMAND ${LLVM_CONFIG_EXECUTABLE} --has-rtti   OUTPUT_VARIABLE LLVM_HAS_RTTI       OUTPUT_STRIP_TRAILING_WHITESPACE)
        string(REGEX MATCH "^([0-9]+)" LLVM_VERSION_MAJOR "${LLVM_VERSION_STRING}")
        set(LLVM_FOUND TRUE)
        set(LLVM_INCLUDE_DIRS ${LLVM_INCLUDEDIR})
        # Match LLVM's own RTTI setting: objects compiled with a different
        # -fno-rtti setting than libLLVM cause undefined typeinfo/vtable refs.
        if(LLVM_HAS_RTTI STREQUAL "YES")
            set(LLVM_ENABLE_RTTI TRUE)
        else()
            set(LLVM_ENABLE_RTTI FALSE)
        endif()
        # Full library set for the final link (used by the driver target).
        set(LLVM_LIBRARIES ${LLVM_LIBS})
    endif()

    if(LLVM_FOUND)
        message(STATUS "Found LLVM: ${LLVM_VERSION_MAJOR} (${LLVM_CONFIG_EXECUTABLE})")
    else()
        message(WARNING
            "LLVM not found. The compiler backend will not be built.\n"
            "Install LLVM 15+ or set LLVM_DIR to your LLVM installation.")
    endif()
endif()

# ─── Find Clang C++ API (optional; for the C/ObjC FFI interop stage) ──────
# Ubuntu's clang packages ship an incomplete *static* component set (the
# clangBasic archive is absent), so importing the full Clang target set can
# fail configuration. The FFI interop stage that actually needs libclang opts
# in explicitly; code generation only needs the LLVM C++ API and the clang
# driver (detected below), neither of which requires this.
option(SUKICODE_USE_CLANG_CPP_API "Use the libclang C++ API (C/ObjC FFI interop)" OFF)
set(SUKI_HAS_CLANG FALSE)
if(SUKICODE_USE_CLANG_CPP_API AND LLVM_FOUND)
    find_package(Clang CONFIG QUIET)
    if(Clang_FOUND)
        set(SUKI_HAS_CLANG TRUE)
    else()
        message(WARNING "SUKICODE_USE_CLANG_CPP_API=ON but libclang was not found.")
    endif()
endif()

# The clang *driver executable* is used as the linker and as the C compiler for
# the runtime/stdlib objects during the `suki build` stage.
find_program(SUKI_CLANG_EXECUTABLE
    NAMES clang-20 clang-19 clang-18 clang-17 clang-16 clang-15
          clang++-20 clang++-19 clang++-18 clang++-17 clang++-16 clang++
    PATHS /usr/bin /usr/local/bin /opt/homebrew/bin
)
if(SUKI_CLANG_EXECUTABLE)
    message(STATUS "Found clang driver: ${SUKI_CLANG_EXECUTABLE}")
else()
    message(WARNING "clang driver not found; the `suki build` link stage will be unavailable.")
endif()

# ─── Platform Detection ───────────────────────────────────────────────────
if(WIN32)
    add_compile_definitions(SUKI_PLATFORM_WINDOWS=1)
    set(SUKI_PLATFORM "windows")
elseif(APPLE)
    add_compile_definitions(SUKI_PLATFORM_MACOS=1)
    set(SUKI_PLATFORM "macos")
elseif(UNIX)
    add_compile_definitions(SUKI_PLATFORM_LINUX=1)
    set(SUKI_PLATFORM "linux")
endif()

# Derive the host architecture from the actual build target instead of a
# hardcoded value, so this variable stays correct on every platform
# (e.g. aarch64 / arm), not just x86_64. The real target detection used by
# code generation lives in TargetInfo.cpp (compiler builtins __x86_64__ /
# __aarch64__ ...); this is only an informational variable.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    set(SUKI_ARCH "x86_64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$")
    set(SUKI_ARCH "aarch64")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(armv7|armv6|arm)$")
    set(SUKI_ARCH "arm")
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(i[3-6]86)$")
    set(SUKI_ARCH "x86")
else()
    set(SUKI_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    add_compile_definitions(SUKI_ARCH_64BIT=1)
else()
    add_compile_definitions(SUKI_ARCH_32BIT=1)
endif()

# ─── Warning flags ────────────────────────────────────────────────────────
if(MSVC)
    add_compile_options(/W4 /WX- /wd4100 /wd4127 /wd4244 /wd4458 /wd4702)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)
endif()

# ─── Subdirectories ───────────────────────────────────────────────────────
add_subdirectory(src)

if(SUKICODE_BUILD_TOOLS)
    add_subdirectory(tools)
endif()

if(SUKICODE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
