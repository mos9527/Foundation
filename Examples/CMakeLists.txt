cmake_minimum_required(VERSION 3.26)

include(FetchContent)
FetchContent_MakeAvailable(argh)

add_subdirectory(Shaders)
add_subdirectory(Samples)

add_library(Example_Common STATIC Examples.cpp)
target_link_libraries(Example_Common PUBLIC Foundation_RenderCore Foundation_RHIVulkan Foundation_RenderUtils ThirdParty_STB argh)
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

if (NOT ANDROID)
    add_executable(Example_JobSystem "JobSystem.cpp")
    target_link_libraries(Example_JobSystem PRIVATE Foundation_Core)
    if (FOUNDATION_WITH_TESTS)
        add_test(NAME JobSystemSmoke COMMAND Example_JobSystem)
    endif()
endif()

add_subdirectory(Renderer)
