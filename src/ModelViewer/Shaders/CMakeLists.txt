include(slangc_compile)
slangc_compile(
    Shader_Mesh
    SOURCE          "${CMAKE_CURRENT_LIST_DIR}/Mesh.slang"
    OUTPUT_DIR      "${SHADER_BUILD_DIR}"
    TARGETS         spirv       spirv
    PROFILES        spirv_1_4   spirv_1_4 
    ENTRY_POINTS    vertMain    fragMain
)
