cmake_minimum_required(VERSION 3.10.0)
project("jni")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(jni SHARED jni.cc)

target_compile_options(jni PRIVATE -O3)

target_compile_options(jni PRIVATE -flto=thin)
target_link_options(jni PRIVATE -flto=thin)

target_compile_options(jni PRIVATE -fsanitize=cfi -fsanitize-cfi-cross-dso -fvisibility=hidden)
target_link_options(jni PRIVATE -fsanitize=cfi -fsanitize-cfi-cross-dso)

if(NOT ANDROID_ABI MATCHES "armeabi-v7a|x86")
    target_compile_options(jni PRIVATE -mbranch-protection=standard)
endif()

target_compile_options(jni PRIVATE -fstack-protector-strong)

target_link_libraries(jni PRIVATE log)