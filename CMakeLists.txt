cmake_minimum_required(VERSION 3.20)

# SukiCode Programming Language Toolchain
# A modern systems programming language with LLVM backend
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

# Position-independent code for shared libraries
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# ─── Options ───────────────────────────────────────────────────────────────
option(SUKICODE_BUILD_TESTS "Build test suite" ON)
option(SUKICODE_BUILD_TOOLS "Build tools (suki-lsp, suki-fmt)" ON)
option(SUKICODE_BUILD_SUKIPM "Build package manager" ON)
option(SUKICODE_USE_SYSTEM_LLVM "Use system LLVM installation" ON)

# ─── Find LLVM ─────────────────────────────────────────────────────────────
# Try to find LLVM via cmake config first, then fall back to llvm-config
if(SUKICODE_USE_SYSTEM_LLVM)
    # Try standard CMake config
    find_package(LLVM CONFIG QUIET)

    if(NOT LLVM_FOUND)
        # Try finding via llvm-config
        find_program(LLVM_CONFIG_EXECUTABLE
            NAMES llvm-config llvm-config-18 llvm-config-17 llvm-config-16 llvm-config-15
            PATHS
                /usr/bin
                /usr/local/bin
                /opt/homebrew/bin
                "C:/Program Files/LLVM/bin"
                "$ENV{LLVM_DIR}/bin"
        )

        if(LLVM_CONFIG_EXECUTABLE)
            message(STATUS "Found llvm-config: ${LLVM_CONFIG_EXECUTABLE}")
            execute_process(
                COMMAND ${LLVM_CONFIG_EXECUTABLE} --prefix
                OUTPUT_VARIABLE LLVM_PREFIX
                OUTPUT_STRIP_TRAILING_WHITESPACE
            )
            execute_process(
                COMMAND ${LLVM_CONFIG_EXECUTABLE} --version
                OUTPUT_VARIABLE LLVM_VERSION_STRING
                OUTPUT_STRIP_TRAILING_WHITESPACE
            )
            execute_process(
                COMMAND ${LLVM_CONFIG_EXECUTABLE} --libdir
                OUTPUT_VARIABLE LLVM_LIBDIR
                OUTPUT_STRIP_TRAILING_WHITESPACE
            )
            execute_process(
                COMMAND ${LLVM_CONFIG_EXECUTABLE} --includedir
                OUTPUT_VARIABLE LLVM_INCLUDEDIR
                OUTPUT_STRIP_TRAILING_WHITESPACE
            )
            execute_process(
                COMMAND ${LLVM_CONFIG_EXECUTABLE} --system-libs --libs all
                OUTPUT_VARIABLE LLVM_LIBS
                OUTPUT_STRIP_TRAILING_WHITESPACE
            )
            execute_process(
                COMMAND ${LLVM_CONFIG_EXECUTABLE} --ldflags
                OUTPUT_VARIABLE LLVM_LDFLAGS
                OUTPUT_STRIP_TRAILING_WHITESPACE
            )
            execute_process(
                COMMAND ${LLVM_CONFIG_EXECUTABLE} --cxxflags
                OUTPUT_VARIABLE LLVM_CXXFLAGS
                OUTPUT_STRIP_TRAILING_WHITESPACE
            )

            # Build LLVM_LIBRARIES from llvm-config output
            # Strip the flags from libs output (e.g., -lfoo -> foo)
            string(REPLACE "-l" "" LLVM_LIBS_STRIPPED "${LLVM_LIBS}")
            string(REPLACE " " ";" LLVM_LIBS_LIST "${LLVM_LIBS_STRIPPED}")

            set(LLVM_FOUND TRUE)
            set(LLVM_INCLUDE_DIRS ${LLVM_INCLUDEDIR})
            set(LLVM_DEFINITIONS "")
            # We'll use llvm-config output directly
        endif()
    endif()

    if(LLVM_FOUND)
        message(STATUS "Found LLVM: ${LLVM_DIR}")
        message(STATUS "LLVM version: ${LLVM_VERSION_MAJOR}.${LLVM_VERSION_MINOR}")

        # Get LLVM version
        if(NOT DEFINED LLVM_VERSION_MAJOR AND DEFINED LLVM_VERSION_STRING)
            string(REGEX MATCH "^([0-9]+)" LLVM_VERSION_MAJOR "${LLVM_VERSION_STRING}")
        endif()

        # Add LLVM includes
        if(TARGET LLVMCore)
            # Modern LLVM CMake targets available
            set(SUKI_LLVM_TARGETS
                LLVMCore LLVMSupport LLVMX86CodeGen LLVMX86AsmParser
                LLVMX86Desc LLVMX86Info LLVMARMCodeGen LLVMARMAsmParser
                LLVMARMDesc LLVMARMInfo LLVMAArch64CodeGen LLVMAArch64AsmParser
                LLVMAArch64Desc LLVMAArch64Info
                LLVMRISCVCodeGen LLVMRISCVAsmParser LLVMRISCVDesc LLVMRISCVInfo
                LLVMWebAssemblyCodeGen LLVMWebAssemblyAsmParser
                LLVMWebAssemblyDesc LLVMWebAssemblyInfo
                LLVMipo LLVMScalarOpts LLVMInstCombine LLVMTransformUtils
                LLVMAnalysis LLVMCodeGen LLVMSelectionDAG LLVMAsmPrinter
                LLVMMC LLVMMCParser LLVMMCDisassembler LLVMObject
                LLVMBitReader LLVMBitWriter LLVMTarget
                LLVMLinker LLVMLTO
            )
            foreach(target ${SUKI_LLVM_TARGETS})
                if(TARGET ${target})
                    list(APPEND LLVM_LINK_TARGETS ${target})
                endif()
            endforeach()
        else()
            # Use library files directly
            message(STATUS "Using LLVM libraries from: ${LLVM_LIBRARY_DIRS}")
        endif()
    else()
        message(WARNING
            "LLVM not found. The compiler backend will not be built.\n"
            "Install LLVM 15+ or set LLVM_DIR to your LLVM installation.\n"
            "  Ubuntu/Debian: sudo apt install llvm-dev clang-dev\n"
            "  macOS: brew install llvm\n"
            "  Windows: Download from https://releases.llvm.org/\n"
        )
    endif()
endif()

# ─── Find Clang libraries (for C/ObjC interop) ───────────────────────────
if(LLVM_FOUND)
    find_package(Clang CONFIG QUIET)
    if(Clang_FOUND)
        message(STATUS "Found Clang: ${CLANG_CMAKE_DIR}")
        set(SUKI_HAS_CLANG TRUE)
    else()
        message(STATUS "Clang libraries not found - C/ObjC interop will be limited")
        set(SUKI_HAS_CLANG FALSE)
    endif()
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

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    add_compile_definitions(SUKI_ARCH_64BIT=1)
    set(SUKI_ARCH "x86_64")
else()
    add_compile_definitions(SUKI_ARCH_32BIT=1)
    set(SUKI_ARCH "x86")
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
