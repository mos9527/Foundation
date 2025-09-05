include(slangc_compile)
slangc_compile(
    Shader_PSFullscreen
    SOURCE          "${CMAKE_CURRENT_LIST_DIR}/PSFullscreen.slang"
    OUTPUT_DIR      "${SHADER_BUILD_DIR}"
    TARGETS         spirv
    PROFILES        spirv_1_4 
    ENTRY_POINTS    vertMain
)
slangc_compile(
    Shader_PSCopy
    SOURCE          "${CMAKE_CURRENT_LIST_DIR}/PSCopy.slang"
    OUTPUT_DIR      "${SHADER_BUILD_DIR}"
    TARGETS         spirv
    PROFILES        spirv_1_4 
    ENTRY_POINTS    fragMain
)
