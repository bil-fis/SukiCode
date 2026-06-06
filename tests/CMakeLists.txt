# SukiCode test suite
# Uses CTest for integration and a simple test runner for unit tests.

# ─── Unit test for the Lexer ───────────────────────────────────────────────
add_executable(test_lexer
    unit/test_lexer.cpp
)
target_link_libraries(test_lexer PRIVATE sukicompiler)
target_include_directories(test_lexer PRIVATE ${CMAKE_SOURCE_DIR}/src/compiler/..)

add_test(NAME LexerTests COMMAND test_lexer)

# ─── Unit test for the Parser ──────────────────────────────────────────────
add_executable(test_parser
    unit/test_parser.cpp
)
target_link_libraries(test_parser PRIVATE sukicompiler)
target_include_directories(test_parser PRIVATE ${CMAKE_SOURCE_DIR}/src/compiler/..)

add_test(NAME ParserTests COMMAND test_parser)

# ─── Integration test: compile example files ──────────────────────────────
add_test(NAME CompileHelloWorld
    COMMAND sukic --emit-ast ${CMAKE_SOURCE_DIR}/examples/hello_world.suki
)
set_tests_properties(CompileHelloWorld PROPERTIES TIMEOUT 10)

add_test(NAME CompileBasics
    COMMAND sukic --emit-ast ${CMAKE_SOURCE_DIR}/examples/basics.suki
)
set_tests_properties(CompileBasics PROPERTIES TIMEOUT 10)

# ─── Module integration tests ─────────────────────────────────────────────
# Compile and verify moduleTest files
file(GLOB MODULETEST_LEXER ${CMAKE_SOURCE_DIR}/moduleTest/lexer/*.suki)
file(GLOB MODULETEST_PARSER ${CMAKE_SOURCE_DIR}/moduleTest/parser/*.suki)
file(GLOB MODULETEST_INTEGRATION ${CMAKE_SOURCE_DIR}/moduleTest/integration/*.suki)

foreach(test_file ${MODULETEST_LEXER} ${MODULETEST_PARSER})
    get_filename_component(test_name ${test_file} NAME_WE)
    add_test(NAME "ModuleTest_${test_name}"
        COMMAND sukic --emit-ast ${test_file}
    )
    set_tests_properties("ModuleTest_${test_name}" PROPERTIES TIMEOUT 10)
endforeach()

foreach(test_file ${MODULETEST_INTEGRATION})
    get_filename_component(test_name ${test_file} NAME_WE)
    # Integration tests: compile to object file
    add_test(NAME "Integration_${test_name}"
        COMMAND sukic -c -o ${CMAKE_BINARY_DIR}/moduleTest_${test_name}.obj ${test_file}
    )
    set_tests_properties("Integration_${test_name}" PROPERTIES TIMEOUT 30)
endforeach()
