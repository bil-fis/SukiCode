# sukipm — SukiCode 包管理器

add_executable(sukipm
    main.cpp
    Manifest.cpp
    DependencyResolver.cpp
)

target_include_directories(sukipm PRIVATE ${CMAKE_SOURCE_DIR}/src/compiler/..)

if(MSVC)
    target_compile_options(sukipm PRIVATE /utf-8)
endif()
