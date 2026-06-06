# SukiCode Compiler (sukic)
# The main compiler executable: lexer → parser → AST → IR → object code → link

# ─── Compiler library (reusable by LSP, tools, tests) ─────────────────────
add_library(sukicompiler STATIC
    # Lexer
    lexer/Token.cpp
    lexer/Lexer.cpp
    # Parser
    parser/Parser.cpp
    # AST
    ast/ASTNode.cpp
    ast/ASTPrinter.cpp
    # Semantic Analysis
    sema/Sema.cpp
    sema/TypeChecker.cpp
    sema/Type.cpp
    sema/SymbolTable.cpp
    # IR Generation
    codegen/IRGenerator.cpp
    codegen/ObjectEmitter.cpp
    # Diagnostics
    diag/Diagnostic.cpp
)

target_include_directories(sukicompiler PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

# MSVC UTF-8 support for Chinese comments
if(MSVC)
    target_compile_options(sukicompiler PRIVATE /utf-8)
endif()

# LLVM integration
if(LLVM_FOUND)
    target_include_directories(sukicompiler PUBLIC ${LLVM_INCLUDE_DIRS})
    target_compile_definitions(sukicompiler PUBLIC SUKI_HAS_LLVM=1)

    if(DEFINED LLVM_LINK_TARGETS)
        target_link_libraries(sukicompiler PUBLIC ${LLVM_LINK_TARGETS})
    elseif(DEFINED LLVM_LIBDIR)
        # Link via llvm-config output
        target_link_directories(sukicompiler PUBLIC ${LLVM_LIBDIR})
        # Note: actual linking happens in the executable target
    endif()

    # Remove LLVM's RTTI requirement to match our code
    # MSVC: /GR-, GCC/Clang: -fno-rtti
    if(NOT LLVM_ENABLE_RTTI)
        if(MSVC)
            target_compile_options(sukicompiler PRIVATE /GR-)
        else()
            target_compile_options(sukicompiler PRIVATE -fno-rtti)
        endif()
    endif()
endif()

# ─── Compiler executable ──────────────────────────────────────────────────
add_executable(sukic main.cpp)
target_link_libraries(sukic PRIVATE sukicompiler)

if(MSVC)
    target_compile_options(sukic PRIVATE /utf-8)
endif()
