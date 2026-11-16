# Vendored upstream LOOM/OCTI, including unmodified solver interfaces.
file(GLOB_RECURSE LOOM_SOURCES CONFIGURE_DEPENDS
    octi/*.cpp loom/*.cpp shared/*.cpp dot/*.cpp util/*.cpp)
# OctiMain is included once by native_layout.cpp to reuse drawComp directly.
list(FILTER LOOM_SOURCES EXCLUDE REGEX "/octi/OctiMain\\.cpp$")
set_source_files_properties(loom/LoomMain.cpp PROPERTIES COMPILE_DEFINITIONS "main=upstreamLoomMain")
set(VERSION_GIT_FULL "embedded-upstream")
configure_file(octi/_config.h.in octi/_config.h)
configure_file(loom/_config.h.in loom/_config.h)
add_library(transitmap_loom STATIC ${LOOM_SOURCES} native_layout.cpp)
target_include_directories(transitmap_loom PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR})
target_compile_definitions(transitmap_loom PRIVATE UTIL_LOGLVL=1)
find_package(Threads REQUIRED)
target_link_libraries(transitmap_loom PRIVATE Threads::Threads)
