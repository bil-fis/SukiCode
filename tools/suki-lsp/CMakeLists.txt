# suki-lsp — SukiCode Language Server

add_executable(suki-lsp
    main.cpp
    LSPServer.cpp
)

target_include_directories(suki-lsp PRIVATE ${CMAKE_SOURCE_DIR}/src/compiler/..)
target_link_libraries(suki-lsp PRIVATE sukicompiler)

if(MSVC)
    target_compile_options(suki-lsp PRIVATE /utf-8)
endif()
