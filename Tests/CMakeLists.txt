cmake_minimum_required(VERSION 3.26)

add_executable(Test_Allocator "Allocator.cpp")
target_link_libraries(Test_Allocator PUBLIC Foundation_Core)

add_executable(Test_ShaderReflection "Shader.cpp")
target_link_libraries(Test_ShaderReflection PUBLIC Foundation_RenderCore Foundation_Native)

add_executable(Test_Staging "StagingBuffer.cpp")
target_link_libraries(Test_Staging PUBLIC Foundation_Rendering)

add_executable(Test_SPSC "SPSC.cpp")
target_link_libraries(Test_SPSC PUBLIC Foundation_Atomics)

add_executable(Test_MPMC "MPMC.cpp")
target_link_libraries(Test_MPMC PUBLIC Foundation_Atomics)

add_executable(Test_QueueNonTrivial "QueueNonTrivial.cpp")
target_link_libraries(Test_QueueNonTrivial PUBLIC Foundation_Atomics)

add_executable(Test_Stack "Stack.cpp")
target_link_libraries(Test_Stack PUBLIC Foundation_Atomics)

add_executable(Test_ThreadPool "ThreadPool.cpp")
target_link_libraries(Test_ThreadPool PUBLIC Foundation_Async)

add_executable(Test_Unrom "Unorm.cpp")
target_link_libraries(Test_Unrom PUBLIC Foundation_Math Foundation_Core)

add_executable(Test_Meshoptimizer "Meshoptimizer.cpp")
target_link_libraries(Test_Meshoptimizer PUBLIC ModelViewer_Assets)
add_dependencies(Test_Meshoptimizer ModelViewer_Assets)