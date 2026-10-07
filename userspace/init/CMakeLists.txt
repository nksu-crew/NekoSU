cmake_minimum_required(VERSION 3.10)

project(init C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_executable(init
    init.c
    kmod.c
    hashtable.c
)

target_compile_options(init PRIVATE
    -Os
    -ffunction-sections
    -fdata-sections
)

# Android 内核强制 PIE，静态链接需使用 -static-pie
target_link_options(init PRIVATE
    -static-pie
    -Wl,--gc-sections
    -s
)
