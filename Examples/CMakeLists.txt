cmake_minimum_required(VERSION 3.26)

FetchContent_MakeAvailable(argh)

add_subdirectory(Shaders)
add_subdirectory(Samples)

add_library(Example_Common STATIC Examples.cpp)
target_link_libraries(Example_Common PUBLIC Foundation_Presenter Foundation_RenderCore Foundation_RHIVulkan Foundation_RenderUtils Foundation_Rasterizer Foundation_Pathtracer ThirdParty_STB argh)
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

# Headless offscreen rendering (no SDL window / no Vulkan WSI). Validates the
# present=false Renderer path and works on software backends such as Lavapipe.
add_example(Example_HeadlessTriangle "HeadlessTriangle.cpp")
target_link_libraries(Example_HeadlessTriangle PUBLIC Example_Common)
add_dependencies(Example_HeadlessTriangle Shader_Triangle)

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

# Core-only self-test for the JobGraph scheduler (no RHI/render deps).
add_example(Example_JobGraph "JobGraph.cpp")
target_link_libraries(Example_JobGraph PUBLIC Foundation_Core)

add_example(Example_ImGui "ImGui.cpp")
target_link_libraries(Example_ImGui PUBLIC Example_Common ThirdParty_ImGui)

add_example(Example_MipGeneration "MipGeneration.cpp")
target_link_libraries(Example_MipGeneration PUBLIC Example_Common ThirdParty_ImGui)
add_dependencies(Example_MipGeneration Shader_MipGenerationBlur Examples_SampleFiles)

add_example(Example_MeshShaderHello "MeshShaderHello.cpp")
target_link_libraries(Example_MeshShaderHello PUBLIC Example_Common)
add_dependencies(Example_MeshShaderHello Shader_MeshShaderHelloTask Shader_MeshShaderHelloMesh Shader_MeshShaderHelloFrag)

add_example(Example_MeshShaderHierarchicalLOD "MeshShaderHierarchicalLOD.cpp")
target_link_libraries(Example_MeshShaderHierarchicalLOD PUBLIC Editor_Assets Example_Common)
add_dependencies(Example_MeshShaderHierarchicalLOD Shader_MeshShaderHierarchicalLODMesh Shader_MeshShaderBasicFrag Examples_SampleFiles)

add_example(Example_CIEChromacity "CIEChromacity.cpp")
target_link_libraries(Example_CIEChromacity PUBLIC Example_Common)
add_dependencies(Example_CIEChromacity Shader_CIEChromacity)

add_example(Example_GPUScene "GPUScene.cpp")
target_link_libraries(Example_GPUScene PUBLIC Example_Common Editor_Assets Foundation_Rasterizer Foundation_Pathtracer)
add_dependencies(Example_GPUScene Foundation_Shaders Editor_Data Examples_SampleFiles Shader_PostprocessBasic Shader_PostprocessBasicPT)

add_example(Example_GPUSceneGLTF "GPUSceneGLTF.cpp")
target_link_libraries(Example_GPUSceneGLTF PUBLIC Example_Common Editor_Assets Foundation_Rasterizer Foundation_Pathtracer)
add_dependencies(Example_GPUSceneGLTF Foundation_Shaders Editor_Data Examples_SampleFiles Shader_PostprocessBasic Shader_PostprocessBasicPT)

add_example(Example_GPUSceneStreaming "GPUSceneStreaming.cpp")
target_link_libraries(Example_GPUSceneStreaming PUBLIC Example_Common Editor_Assets Foundation_Rasterizer Foundation_Pathtracer)
add_dependencies(Example_GPUSceneStreaming Foundation_Shaders Editor_Data Examples_SampleFiles Shader_PostprocessBasic Shader_PostprocessBasicPT)

add_example(Example_GPUSceneDeform "GPUSceneDeform.cpp")
target_link_libraries(Example_GPUSceneDeform PUBLIC Example_Common Editor_Assets Foundation_Rasterizer Foundation_Pathtracer)
add_dependencies(Example_GPUSceneDeform Foundation_Shaders Editor_Data Examples_SampleFiles Shader_PostprocessBasic Shader_PostprocessBasicPT)

add_example(Example_HeadlessPathTracer "HeadlessPathTracer.cpp")
target_link_libraries(Example_HeadlessPathTracer PUBLIC Example_Common Editor_Assets Foundation_Rasterizer Foundation_Pathtracer)
add_dependencies(Example_HeadlessPathTracer Foundation_Shaders Editor_Data Examples_SampleFiles Shader_PostprocessBasic Shader_PostprocessBasicPT)

