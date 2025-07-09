cmake_minimum_required(VERSION 3.26)
project(Foundation)

find_package(Vulkan)

add_subdirectory(src/Platform)
add_subdirectory(src/Core)
add_subdirectory(src/Editor)

add_subdirectory(tests)