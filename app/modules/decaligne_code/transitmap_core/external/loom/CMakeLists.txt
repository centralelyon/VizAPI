# Embedded LOOM/OCTI, heuristic paths only. No external solver or CLI dependency.
file(GLOB_RECURSE LOOM_SOURCES CONFIGURE_DEPENDS
    octi/*.cpp loom/*.cpp shared/*.cpp dot/*.cpp util/*.cpp)
add_library(transitmap_loom STATIC ${LOOM_SOURCES} native_layout.cpp)
target_include_directories(transitmap_loom PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(transitmap_loom PRIVATE UTIL_LOGLVL=1)
find_package(Threads REQUIRED)
target_link_libraries(transitmap_loom PRIVATE Threads::Threads)
