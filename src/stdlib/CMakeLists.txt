# SukiCode standard library.
#
# These `.suki` sources are NOT compiled by CMake. The `sukic` driver loads the
# modules requested via `import` at compile time (the file list is baked into the
# driver through SUKI_STDLIB_FILES, set in src/compiler/CMakeLists.txt). They are
# listed here so the build system and IDE tooling are aware of them.

file(GLOB_RECURSE SUKI_STDLIB_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.suki")

# No build target: the sources are inputs to the SukiCode compiler itself.
# `sukipm` / editors can use SUKI_STDLIB_SOURCES to index the library.
set(SUKI_STDLIB_SOURCES ${SUKI_STDLIB_SOURCES} CACHE INTERNAL "" FORCE)
