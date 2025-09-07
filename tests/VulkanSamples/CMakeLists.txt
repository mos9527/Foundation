cmake_minimum_required(VERSION 3.26)

add_executable(Vk_FramesInFlight "16_frames_in_flight.cpp")
target_link_libraries(Vk_FramesInFlight PUBLIC Foundation_Rendering)
slangc_compile(
        Shader_09_ShaderBase
        SOURCE          "${CMAKE_CURRENT_LIST_DIR}/09_shader_base.slang"
        OUTPUT_DIR      "${SHADER_BUILD_DIR}"
)
add_dependencies(Vk_FramesInFlight Shader_09_ShaderBase)