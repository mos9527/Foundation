cmake_minimum_required(VERSION 3.26)

add_executable(Test_Allocator "Allocator.cpp" )
target_link_libraries(Test_Allocator PUBLIC Foundation_Runtime)

add_executable(Test_RenderPass "RenderPass.cpp")
target_link_libraries(Test_RenderPass PUBLIC Foundation_Renderer)
