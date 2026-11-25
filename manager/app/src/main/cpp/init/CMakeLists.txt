cmake_minimum_required(VERSION 3.16)

project(init
    VERSION 1.0.0
    DESCRIPTION "NekoSU userspace init implement."
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(init src/main.cpp src/perinit.cpp)

set_target_properties(init PROPERTIES
    OUTPUT_NAME "init"
    PREFIX "lib"
    SUFFIX ".so"
    POSITION_INDEPENDENT_CODE ON
)

target_compile_options(init PRIVATE
    -fno-exceptions
    -fno-rtti
)

target_link_options(init PRIVATE
    -static
    -s
)