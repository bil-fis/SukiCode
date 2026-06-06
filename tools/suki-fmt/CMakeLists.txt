# suki-fmt — SukiCode 代码格式化工具

add_executable(suki-fmt
    main.cpp
    Formatter.cpp
)

target_include_directories(suki-fmt PRIVATE ${CMAKE_SOURCE_DIR}/src/compiler/..)
target_link_libraries(suki-fmt PRIVATE sukicompiler)

if(MSVC)
    target_compile_options(suki-fmt PRIVATE /utf-8)
endif()
