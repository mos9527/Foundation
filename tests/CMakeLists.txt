cmake_minimum_required(VERSION 3.26)

add_executable(Test_Allocator "Allocator.cpp")

target_link_libraries(Test_Allocator PUBLIC Foundation_Core)
