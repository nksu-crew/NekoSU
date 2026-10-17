cmake_minimum_required(VERSION 3.10.0)
project("nkjni")

# Shared kernel/userspace UAPI headers (repo-root uapi/), included as
# <uapi/ioctl.h> by the JNI layer.  include/uapi is a symlink into the
# repository-root uapi/ directory (the single source of truth), following the
# same pattern as neko/ksud.  Add this local include/ directory rather
# than the repo root so only the uapi headers are exposed to the compiler.
include_directories("${CMAKE_CURRENT_SOURCE_DIR}/include")

add_subdirectory(libjni)
