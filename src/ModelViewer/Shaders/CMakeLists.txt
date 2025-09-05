set(SHADER_BUILD_DIR "${DERIVED_DATA_DIR}/shaders")
file(MAKE_DIRECTORY ${SHADER_BUILD_DIR})

include(slangc_compile)
slangc_compile(
    Shader_GBuffer
    SOURCE          "${CMAKE_CURRENT_LIST_DIR}/GBuffer.slang"
    OUTPUT_DIR      "${SHADER_BUILD_DIR}"
    TARGETS         spirv
    PROFILES        spirv_1_4 
    ENTRY_POINTS    vertMain
)
add_dependencies(ModelViewer Shader_GBuffer)
