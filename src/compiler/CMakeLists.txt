# SukiCode Compiler (sukic)
# Frontend pipeline: lexer → parser → AST → macro → sema → IR → codegen.
# Built incrementally; the driver executable (sukic) is added once the
# pipeline stages exist.

add_library(sukicompiler STATIC
    # Diagnostics (stage 0 foundation, used by every later stage)
    diag/DiagnosticEngine.cpp
    # Lexer (stage 1)
    lexer/Lexer.cpp
    # Parser (stage 2)
    parser/Parser.cpp
    # Semantic analysis (stage 3): type system + analyzer
    sema/Type.cpp
    sema/Sema.cpp
    # Code generation (stage 4): target abstraction + IR generation
    codegen/TargetInfo.cpp
    codegen/IRGeneratorImpl.cpp
    codegen/TypeLayout.cpp
)

target_include_directories(sukicompiler PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

# Frontend driver executable. Links only the frontend library so it builds and
# runs even when LLVM is not installed (codegen stages are added later).
add_executable(sukic main.cpp)
target_link_libraries(sukic PRIVATE sukicompiler)

# Give the driver the discovered clang path for the link stage.
if(SUKI_CLANG_EXECUTABLE)
    target_compile_definitions(sukic PRIVATE SUKI_CLANG_DRIVER="${SUKI_CLANG_EXECUTABLE}")
endif()

# Prefer lld for the final link when it is installed. The driver then passes
# `-fuse-ld=lld` to the clang driver, which still resolves crt/libc paths while
# lld performs the actual link (BLK-B).
find_program(SUKI_LD_LLD_EXECUTABLE NAMES ld.lld)
if(SUKI_LD_LLD_EXECUTABLE)
    target_compile_definitions(sukic PRIVATE SUKI_PREFER_LLD=1)
    message(STATUS "Found ld.lld: `suki run` will link through lld")
endif()
# BLK-B: precompile the C runtime into an object at build time so `suki run`
# links a ready-made runtime object instead of asking clang to *compile* C at
# link time. The driver then only links (optionally through lld).
# Use the host's native object-file extension (e.g. `.obj` on Windows) so the
# output matches what the platform's linker expects — keeps the build portable.
set(SUKI_RUNTIME_O "${CMAKE_CURRENT_BINARY_DIR}/runtime${CMAKE_C_OUTPUT_EXTENSION}")
add_custom_command(
    OUTPUT "${SUKI_RUNTIME_O}"
    COMMAND ${CMAKE_C_COMPILER} -c
            -I "${CMAKE_SOURCE_DIR}/src/runtime"
            "${CMAKE_SOURCE_DIR}/src/runtime/runtime.c"
            -o "${SUKI_RUNTIME_O}"
    DEPENDS "${CMAKE_SOURCE_DIR}/src/runtime/runtime.c"
            "${CMAKE_SOURCE_DIR}/src/runtime/runtime.h"
    COMMENT "Precompiling SukiCode C runtime -> runtime.o"
    VERBATIM)
add_custom_target(suki_runtime_o DEPENDS "${SUKI_RUNTIME_O}")
add_dependencies(sukic suki_runtime_o)
target_compile_definitions(sukic PRIVATE
    SUKI_RUNTIME_OBJECT="${SUKI_RUNTIME_O}"
    # 交叉编译目标时，驱动会按 --target 即时编译 runtime.c 作为该目标的运行时
    # 对象，因此需要 runtime.c 源文件与其包含目录的绝对路径。
    SUKI_RUNTIME_SOURCE="${CMAKE_SOURCE_DIR}/src/runtime/runtime.c"
    SUKI_RUNTIME_INCLUDE="${CMAKE_SOURCE_DIR}/src/runtime")

# 标准库：目录 + 模块文件清单，供驱动在编译期按 `import` 解析（见 main.cpp
# 的 loadImportedStdlib）。文件清单在配置期 glob 得到，新增 .suki 模块后需重跑 cmake。
file(GLOB_RECURSE SUKI_STDLIB_FILES "${CMAKE_SOURCE_DIR}/src/stdlib/*.suki")
string(JOIN "|" SUKI_STDLIB_FILES_STR ${SUKI_STDLIB_FILES})
target_compile_definitions(sukic PRIVATE
    SUKI_STDLIB_DIR="${CMAKE_SOURCE_DIR}/src/stdlib"
    SUKI_STDLIB_FILES="${SUKI_STDLIB_FILES_STR}")

if(MSVC)
    target_compile_options(sukicompiler PRIVATE /utf-8)
    target_compile_options(sukic PRIVATE /utf-8)
endif()

# LLVM integration (used from stage 4 onward)
if(LLVM_FOUND)
    # The static library only needs LLVM headers; the real library link happens
    # in the driver executable below.
    target_include_directories(sukicompiler PUBLIC ${LLVM_INCLUDE_DIRS})
    target_compile_definitions(sukicompiler PUBLIC SUKI_HAS_LLVM=1)

    # Match LLVM's RTTI setting (detected at top level). Mixing -fno-rtti
    # objects with an RTTI-enabled libLLVM breaks linking with undefined
    # symbols to LLVM's typeinfo/vtables.
    if(NOT LLVM_ENABLE_RTTI)
        if(MSVC)
            target_compile_options(sukicompiler PUBLIC /GR-)
        else()
            target_compile_options(sukicompiler PUBLIC -fno-rtti)
        endif()
    endif()

    # Link LLVM into the driver.
    if(LLVM_LINK_TARGETS)
        target_link_libraries(sukic PRIVATE ${LLVM_LINK_TARGETS})
    elseif(LLVM_LIBRARIES)
        if(DEFINED LLVM_LIBDIR)
            target_link_directories(sukic PRIVATE ${LLVM_LIBDIR})
        endif()
        target_link_libraries(sukic PRIVATE ${LLVM_LIBRARIES})
    endif()
    if(DEFINED LLVM_SYSTEM_LIBS)
        target_link_libraries(sukic PRIVATE ${LLVM_SYSTEM_LIBS})
    endif()
endif()
