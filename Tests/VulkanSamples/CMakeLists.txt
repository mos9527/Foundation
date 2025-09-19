cmake_minimum_required(VERSION 3.26)

add_executable(16_frames_in_flight "16_frames_in_flight.cpp")
target_link_libraries(16_frames_in_flight PUBLIC glfw Foundation_RHIVulkan)
slangc_compile(
        Shader_09_ShaderBase
        SOURCE          "${CMAKE_CURRENT_LIST_DIR}/09_shader_base.slang"
        OUTPUT_DIR      "${SHADER_BUILD_DIR}"
)
add_dependencies(16_frames_in_flight Shader_09_ShaderBase)
