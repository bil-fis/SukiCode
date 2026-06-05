# SukiCode Standard Library
# Core, System, Concurrency modules compiled from .suki sources
# For now, this is a placeholder — the stdlib is written in SukiCode itself
# and compiled by sukic. The C/C++ stubs here provide runtime support.

add_library(sukistd STATIC
    core/Print.cpp
    core/Assert.cpp
    core/String.cpp
)

target_include_directories(sukistd PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

if(MSVC)
    target_compile_options(sukistd PRIVATE /utf-8)
endif()

install(TARGETS sukistd
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)
