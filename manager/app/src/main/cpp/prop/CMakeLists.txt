cmake_minimum_required(VERSION 3.16)

project(prop
    VERSION 1.0.0
    DESCRIPTION "NekoSU Android prop tool."
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include_directories(${PROJECT_SOURCE_DIR}/include)

add_executable(prop src/main.cpp)

set_target_properties(prop PROPERTIES
    OUTPUT_NAME "prop"
    PREFIX "lib"
    SUFFIX ".so"
    POSITION_INDEPENDENT_CODE ON
)

target_link_options(prop PRIVATE
    -fno-exceptions
    -s
)