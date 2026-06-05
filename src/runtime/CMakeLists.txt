# SukiCode Runtime Library
# ARC memory management, coroutine runtime, thread pool, pool/collection

add_library(sukirt STATIC
    arc/ARC.cpp
    arc/RefCount.cpp
    arc/WeakRef.cpp
    pool/MemoryPool.cpp
    pool/Collection.cpp
    concurrency/ThreadPool.cpp
    concurrency/Coroutine.cpp
)

target_include_directories(sukirt PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

if(MSVC)
    target_compile_options(sukirt PRIVATE /utf-8)
endif()

# Install
install(TARGETS sukirt
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
)
