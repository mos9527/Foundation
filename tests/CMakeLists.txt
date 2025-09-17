cmake_minimum_required(VERSION 3.26)

add_executable(Test_Allocator "Allocator.cpp"
        Staging.cpp)
target_link_libraries(Test_Allocator PUBLIC Foundation_Runtime)

add_executable(Test_Misc "Misc.cpp" )
target_link_libraries(Test_Misc PUBLIC Foundation_Runtime)

add_executable(Test_ShaderReflection "Reflection.cpp")
target_link_libraries(Test_ShaderReflection PUBLIC Foundation_Rendering)

add_executable(Test_CircularBuffer "RingBuffer.cpp")
target_link_libraries(Test_CircularBuffer PUBLIC Foundation_Runtime)

add_executable(Test_Staging "Staging.cpp")
target_link_libraries(Test_Staging PUBLIC Foundation_Rendering)

add_subdirectory(VulkanSamples)