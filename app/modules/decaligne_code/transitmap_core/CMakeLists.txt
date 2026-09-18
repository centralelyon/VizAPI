cmake_minimum_required(VERSION 3.15)

project(TransitMapCore)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(TransitMapCore
    src/core/geometry.cpp
    src/core/shape.cpp
    src/core/projection.cpp
    src/core/camera.cpp

    src/render/render_geometry.cpp
    src/render/style.cpp

    src/api/core_api.cpp

    src/obs/region.cpp
    src/obs/line.cpp

    src/io/route_directions.cpp
    src/io/directional.cpp
    src/io/loader.cpp
    src/io/shape_loom_export.cpp
)

target_include_directories(
    TransitMapCore
    PUBLIC
        src
        external/include
)

target_compile_definitions(
    TransitMapCore
    PRIVATE
        TRANSIT_HEADLESS=1
)

add_executable(
    transitmap-core
    src/api/core_cli.cpp
)

target_link_libraries(
    transitmap-core
    PRIVATE
        TransitMapCore
)