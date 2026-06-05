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
# These tests verify that the compiler can parse example programs
add_test(NAME CompileHelloWorld
    COMMAND sukic --emit-ast ${CMAKE_SOURCE_DIR}/examples/hello_world.suki
)
set_tests_properties(CompileHelloWorld PROPERTIES
    TIMEOUT 10
)
