cmake_minimum_required(VERSION 3.26)

add_executable(Foundation_Editor "Main.cpp")

target_link_libraries(Foundation_Editor PUBLIC Foundation_Core)
target_link_libraries(Foundation_Editor PUBLIC Foundation_Platform)
