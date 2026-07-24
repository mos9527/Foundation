cmake_minimum_required(VERSION 3.26)

add_library(Example_Renderer STATIC Examples.cpp)
target_link_libraries(Example_Renderer PUBLIC Example_Common Foundation_GPUScene Foundation_Rasterizer Foundation_Pathtracer)
target_include_directories(Example_Renderer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

add_example(ExampleRenderer_GLTF "GLTF.cpp")
target_link_libraries(ExampleRenderer_GLTF PUBLIC Example_Common Editor_Assets Example_Renderer)
add_dependencies(ExampleRenderer_GLTF Foundation_Shaders Editor_Data Examples_SampleFiles Shader_PostprocessBasic)

add_example(ExampleRenderer_Dynamic "Dynamic.cpp")
target_link_libraries(ExampleRenderer_Dynamic PUBLIC Example_Common Example_Renderer)
add_dependencies(ExampleRenderer_Dynamic Foundation_Shaders Editor_Data Shader_PostprocessBasic Shader_DynamicGerstner)

if (FOUNDATION_WITH_JOLT_EXAMPLES) 
    set(TARGET_UNIT_TESTS OFF CACHE BOOL "" FORCE)
    set(TARGET_HELLO_WORLD OFF CACHE BOOL "" FORCE)
    set(TARGET_PERFORMANCE_TEST OFF CACHE BOOL "" FORCE)
    set(TARGET_SAMPLES OFF CACHE BOOL "" FORCE)
    set(TARGET_VIEWER OFF CACHE BOOL "" FORCE)
    set(USE_ASSERTS OFF CACHE BOOL "" FORCE)
    set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)
    set(CPP_RTTI_ENABLED OFF CACHE BOOL "" FORCE)

    FetchContent_MakeAvailable(JoltPhysics)

    add_library(Example_Jolt STATIC
        Jolt/JoltPhysicsWorld.cpp
        Jolt/JoltGPUScene.cpp)
    target_include_directories(Example_Jolt PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${JoltPhysics_SOURCE_DIR}/..)
    target_link_libraries(Example_Jolt PUBLIC Example_Common Jolt Example_Renderer)

    add_example(ExampleRenderer_JoltPhysics "JoltPhysics.cpp")
    target_link_libraries(ExampleRenderer_JoltPhysics PUBLIC Example_Jolt)
    add_dependencies(ExampleRenderer_JoltPhysics Foundation_Shaders Editor_Data Shader_PostprocessBasic Shader_DynamicGerstner)
endif()
