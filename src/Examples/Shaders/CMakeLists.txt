include(slangc_compile)
slangc_compile(
    Shader_Triangle
    SOURCE          "${CMAKE_CURRENT_LIST_DIR}/Triangle.slang"
    OUTPUT_DIR      "${SHADER_BUILD_DIR}"
    TARGETS         spirv      spirv
    PROFILES        spirv_1_4  spirv_1_4 
    ENTRY_POINTS    vertMain   fragMain
)
slangc_compile(
    Shader_SDF2D
    SOURCE          "${CMAKE_CURRENT_LIST_DIR}/SDF2D.slang"
    OUTPUT_DIR      "${SHADER_BUILD_DIR}"
    TARGETS         spirv
    PROFILES        spirv_1_4 
    ENTRY_POINTS    fragMain
)
