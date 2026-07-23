cmake_minimum_required(VERSION 3.26)

include(FetchContent)
FetchContent_MakeAvailable(argh)

add_subdirectory(Shaders)
add_subdirectory(Samples)

add_library(Example_Common STATIC Examples.cpp)
target_link_libraries(Example_Common PUBLIC Foundation_RenderCore Foundation_RHIVulkan Foundation_RenderUtils Foundation_Rasterizer Foundation_Pathtracer ThirdParty_STB argh)
target_include_directories(Example_Common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if (ANDROID)
    macro(add_example name source)
        add_library(${name} SHARED ${source})
    endmacro()
else()
    macro(add_example name source)
        add_executable(${name} ${source})
    endmacro()
endif()

add_example(Example_Triangle "Triangle.cpp")
target_link_libraries(Example_Triangle PUBLIC Example_Common)
add_dependencies(Example_Triangle Shader_Triangle)

add_example(Example_SDF2D "SDF2D.cpp")
target_link_libraries(Example_SDF2D PUBLIC Example_Common)
add_dependencies(Example_SDF2D Shader_SDF2D)

add_example(Example_MandelbrotCompute "MandelbrotCompute.cpp")
target_link_libraries(Example_MandelbrotCompute PUBLIC Example_Common)
add_dependencies(Example_MandelbrotCompute Shader_MandelbrotCompute)

add_example(Example_BindlessSimple "BindlessSimple.cpp")
target_link_libraries(Example_BindlessSimple PUBLIC Example_Common)
add_dependencies(Example_BindlessSimple Shader_BindlessSimple)

add_example(Example_DebugText "DebugText.cpp")
target_link_libraries(Example_DebugText PUBLIC Example_Common)

FetchContent_MakeAvailable(imgui)
add_example(Example_ImGui "ImGui.cpp")
target_link_libraries(Example_ImGui PUBLIC Example_Common ThirdParty_ImGui)
target_sources(Example_ImGui PRIVATE "${imgui_SOURCE_DIR}/imgui_demo.cpp")

add_example(Example_MipGeneration "MipGeneration.cpp")
target_link_libraries(Example_MipGeneration PUBLIC Example_Common ThirdParty_ImGui)
add_dependencies(Example_MipGeneration Shader_MipGenerationBlur Examples_SampleFiles)

add_example(Example_AutodiffSimple "AutodiffSimple.cpp")
target_link_libraries(Example_AutodiffSimple PUBLIC Example_Common ThirdParty_ImGui)
add_dependencies(Example_AutodiffSimple Shader_AutodiffSimple Examples_SampleFiles)

add_example(Example_CIEChromacity "CIEChromacity.cpp")
target_link_libraries(Example_CIEChromacity PUBLIC Example_Common)
add_dependencies(Example_CIEChromacity Shader_CIEChromacity)

add_example(Example_GPUScene "GPUScene.cpp")
target_link_libraries(Example_GPUScene PUBLIC Example_Common Editor_Assets Foundation_Rasterizer Foundation_Pathtracer)
add_dependencies(Example_GPUScene Foundation_Shaders Editor_Data Examples_SampleFiles Shader_PostprocessBasic)

add_example(Example_DynamicGeometry "DynamicGeometry.cpp")
target_link_libraries(Example_DynamicGeometry PUBLIC Example_Common Foundation_Rasterizer)
add_dependencies(Example_DynamicGeometry Foundation_Shaders Editor_Data Shader_PostprocessBasic Shader_DynamicGerstner)

if (FOUNDATION_WITH_JOLT_EXAMPLES)
    
    
    set(TARGET_UNIT_TESTS OFF CACHE BOOL "" FORCE)
    set(TARGET_HELLO_WORLD OFF CACHE BOOL "" FORCE)
    set(TARGET_PERFORMANCE_TEST OFF CACHE BOOL "" FORCE)
    set(TARGET_SAMPLES OFF CACHE BOOL "" FORCE)
    set(TARGET_VIEWER OFF CACHE BOOL "" FORCE)
    set(USE_ASSERTS OFF CACHE BOOL "" FORCE)
    set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(JoltPhysics)
    
    add_example(Example_PhysicsJolt "PhysicsJolt.cpp")
    target_include_directories(Example_PhysicsJolt PUBLIC ${JoltPhysics_SOURCE_DIR}/..)
    target_link_libraries(Example_PhysicsJolt PUBLIC Example_Common Foundation_Rasterizer Jolt)
    add_dependencies(Example_PhysicsJolt Foundation_Shaders Editor_Data Shader_PostprocessBasic Shader_DynamicGerstner)
endif()
