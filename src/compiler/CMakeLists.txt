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
# Path to the C runtime source, linked into `suki run` executables.
target_compile_definitions(sukic PRIVATE
    SUKI_RUNTIME_SOURCE="${CMAKE_SOURCE_DIR}/src/runtime/runtime.c")

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
