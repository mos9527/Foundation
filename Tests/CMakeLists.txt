cmake_minimum_required(VERSION 3.26)

add_executable(Test_Allocator "Allocator.cpp")
target_link_libraries(Test_Allocator PUBLIC Foundation_Core)

add_executable(Test_ShaderReflection "Shader.cpp")
target_link_libraries(Test_ShaderReflection PUBLIC Foundation_RenderCore Foundation_Native)

add_executable(Test_Staging "StagingBuffer.cpp")
target_link_libraries(Test_Staging PUBLIC Foundation_Rendering)

add_executable(Test_SPSC "SPSC.cpp")
target_link_libraries(Test_SPSC PUBLIC Foundation_Atomic)

add_subdirectory(VulkanSamples)