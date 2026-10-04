# suki-doc — SukiCode 文档生成器

add_executable(suki-doc main.cpp)

if(MSVC)
    target_compile_options(suki-doc PRIVATE /utf-8)
endif()
