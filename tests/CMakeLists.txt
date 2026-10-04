# SukiCode test suite.
#
# Two layers:
#   * unit tests  — link the compiler libraries directly and assert on
#                   diagnostics / IR text / runtime behaviour.
#   * CTest cases — drive the `sukic` driver over the .suki corpus.
#
# The canonical "run everything" entry point is ../../run_all_tests.sh, which
# builds these targets and then exercises the whole corpus (including the
# negative-diagnostic cases that CTest cannot express as a pass/fail pair).

# ─── unit tests ────────────────────────────────────────────────────────────
# Lexer and parser coverage (the original two suites).
foreach(unit test_lexer test_parser)
    add_executable(${unit} unit/${unit}.cpp)
    target_link_libraries(${unit} PRIVATE sukicompiler)
    target_include_directories(${unit} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME ${unit} COMMAND ${unit})
    set_tests_properties(${unit} PROPERTIES TIMEOUT 60)
endforeach()

# test_lexer / test_parser already existed; the rest cover the stages added
# later (semantic analysis, lowering, and the C runtime).
foreach(unit test_sema test_codegen)
    add_executable(${unit} unit/${unit}.cpp)
    target_link_libraries(${unit} PRIVATE sukicompiler)
    target_include_directories(${unit} PRIVATE ${CMAKE_SOURCE_DIR}/src)
    add_test(NAME ${unit} COMMAND ${unit})
    set_tests_properties(${unit} PROPERTIES TIMEOUT 60)
endforeach()

# Lowering pulls in LLVM (TypeLayout builds llvm::Type values), so the codegen
# test needs the same libraries the driver links against.
if(LLVM_FOUND)
    if(LLVM_LINK_TARGETS)
        target_link_libraries(test_codegen PRIVATE ${LLVM_LINK_TARGETS})
    elseif(LLVM_LIBRARIES)
        if(DEFINED LLVM_LIBDIR)
            target_link_directories(test_codegen PRIVATE ${LLVM_LIBDIR})
        endif()
        target_link_libraries(test_codegen PRIVATE ${LLVM_LIBRARIES})
    endif()
    if(DEFINED LLVM_SYSTEM_LIBS)
        target_link_libraries(test_codegen PRIVATE ${LLVM_SYSTEM_LIBS})
    endif()
endif()

# The runtime test exercises runtime.c directly, so it compiles the C file as
# part of the target rather than linking the C++ library.
add_executable(test_runtime unit/test_runtime.cpp ${CMAKE_SOURCE_DIR}/src/runtime/runtime.c)
target_include_directories(test_runtime PRIVATE ${CMAKE_SOURCE_DIR})
add_test(NAME test_runtime COMMAND test_runtime)
set_tests_properties(test_runtime PROPERTIES TIMEOUT 60)

# ─── driver-level checks ───────────────────────────────────────────────────
# `sukic` uses subcommands; the older --emit-ast / -o spellings no longer exist.
add_test(NAME CheckHelloWorld
         COMMAND sukic check ${CMAKE_SOURCE_DIR}/examples/hello_world.suki)
add_test(NAME ParseBasics
         COMMAND sukic parse ${CMAKE_SOURCE_DIR}/examples/basics.suki)
set_tests_properties(CheckHelloWorld ParseBasics PROPERTIES TIMEOUT 30)

# Every moduleTest file must at least parse. Generated at configure time so a
# newly added .suki file is picked up without editing this list.
file(GLOB_RECURSE MODULETEST_ALL CONFIGURE_DEPENDS
     ${CMAKE_SOURCE_DIR}/moduleTest/*.suki
     ${CMAKE_SOURCE_DIR}/examples/*.suki)
foreach(src ${MODULETEST_ALL})
    get_filename_component(n ${src} NAME_WE)
    get_filename_component(dir ${src} DIRECTORY)
    get_filename_component(parent ${dir} NAME)
    add_test(NAME "Parse_${parent}_${n}"
             COMMAND sukic parse ${src})
    set_tests_properties("Parse_${parent}_${n}" PROPERTIES TIMEOUT 30)
endforeach()

# ─── codegen cases: run the program and require exit code 0 ─────────────────
# A .suki case signals assertion N by returning N from @main, so exit code 0
# means every assertion inside it held.
file(GLOB CODEGEN_CASES ${CMAKE_SOURCE_DIR}/moduleTest/codegen/*.suki)
foreach(src ${CODEGEN_CASES})
    get_filename_component(n ${src} NAME_WE)
    add_test(NAME "Codegen_${n}" COMMAND sukic run ${src})
    set_tests_properties("Codegen_${n}" PROPERTIES TIMEOUT 120)
endforeach()

# ─── negative cases: the analyser must reject them ─────────────────────────
# Each file pairs with an expected substring of the diagnostic; the shell
# runner performs that comparison, CTest only checks the non-zero exit.
file(GLOB NEGATIVE_CASES ${CMAKE_SOURCE_DIR}/moduleTest/diagnostics/*.suki)
foreach(src ${NEGATIVE_CASES})
    get_filename_component(n ${src} NAME_WE)
    # The driver exits 1 when diagnostics were emitted, which is exactly the
    # outcome a rejection test wants; WILL_FAIL inverts CTest's view so the
    # case passes when the compile is rejected.
    add_test(NAME "Rejects_${n}" COMMAND sukic check ${src})
    set_tests_properties("Rejects_${n}" PROPERTIES TIMEOUT 30 WILL_FAIL TRUE)
endforeach()
