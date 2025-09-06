include(slangc_compile)
slangc_compile(
    Shader_VSFullscreen
    SOURCE          "${CMAKE_CURRENT_LIST_DIR}/VSFullscreen.slang"
    OUTPUT_DIR      "${SHADER_BUILD_DIR}"    
)
slangc_compile(
    Shader_PSCopy
    SOURCE          "${CMAKE_CURRENT_LIST_DIR}/PSCopy.slang"
    OUTPUT_DIR      "${SHADER_BUILD_DIR}"    
)
