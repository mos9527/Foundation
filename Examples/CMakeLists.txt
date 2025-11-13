cmake_minimum_required(VERSION 3.26)

add_subdirectory(Shaders)
add_subdirectory(Samples)

add_library(Example_Common INTERFACE)
target_link_libraries(Example_Common INTERFACE Foundation_RenderCore Foundation_RHIVulkan Foundation_RenderUtils)
target_precompile_headers(Example_Common INTERFACE Examples.hpp)
add_dependencies(Example_Common Examples_SampleFiles)

add_executable(Example_Triangle "Triangle.cpp")
target_link_libraries(Example_Triangle PUBLIC Example_Common)
add_dependencies(Example_Triangle Shader_Triangle)

add_executable(Example_SDF2D "SDF2D.cpp")
target_link_libraries(Example_SDF2D PUBLIC Example_Common)
add_dependencies(Example_SDF2D Shader_SDF2D)

add_executable(Example_MandelbrotCompute "MandelbrotCompute.cpp")
target_link_libraries(Example_MandelbrotCompute PUBLIC Example_Common)
add_dependencies(Example_MandelbrotCompute Shader_MandelbrotCompute)

add_executable(Example_BindlessSimple "BindlessSimple.cpp")
target_link_libraries(Example_BindlessSimple PUBLIC Example_Common)
add_dependencies(Example_BindlessSimple Shader_BindlessSimple)

if (WIN32)
	add_executable(Example_Win32_ScreenCapture "Win32_ScreenCapture.cpp")
	target_link_libraries(Example_Win32_ScreenCapture PUBLIC Example_Common)
	add_dependencies(Example_Win32_ScreenCapture Shader_SimpleCRT Shader_SimpleBloom)
endif()

add_executable(Example_MeshShader "MeshShader.cpp")
target_link_libraries(Example_MeshShader PUBLIC Example_Common)
add_dependencies(Example_MeshShader Shader_MeshShaderTask Shader_MeshShaderMesh Shader_MeshShaderFrag)

add_executable(Example_DebugText "DebugText.cpp")
target_link_libraries(Example_DebugText PUBLIC Example_Common)

add_executable(Example_ManyPass "ManyPass.cpp")
target_link_libraries(Example_ManyPass PUBLIC Example_Common)
add_dependencies(Example_ManyPass Shader_ManyPass)

add_executable(Example_ImGui "ImGui.cpp")
target_link_libraries(Example_ImGui PUBLIC Example_Common ThirdParty_ImGui)

FetchContent_MakeAvailable(stb)
add_executable(Example_MipGeneration "MipGeneration.cpp")
target_link_libraries(Example_MipGeneration PUBLIC Example_Common ThirdParty_ImGui)
target_include_directories(Example_MipGeneration PUBLIC "${stb_SOURCE_DIR}")
add_dependencies(Example_MipGeneration Shader_MipGenerationBlur)

add_executable(Example_Streaming "Streaming.cpp")
target_link_libraries(Example_Streaming PUBLIC Example_Common)
