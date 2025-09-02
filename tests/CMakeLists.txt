cmake_minimum_required(VERSION 3.26)

add_executable(Test_Allocator "Allocator.cpp" )
target_link_libraries(Test_Allocator PUBLIC Foundation_Runtime)

add_executable(Test_Misc "Misc.cpp" )
target_link_libraries(Test_Misc PUBLIC Foundation_Runtime)
