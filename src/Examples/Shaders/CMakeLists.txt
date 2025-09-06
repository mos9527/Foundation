include(slangc_compile)
slangc_compile(
    Shader_Triangle
    SOURCE          "${CMAKE_CURRENT_LIST_DIR}/Triangle.slang"
    OUTPUT_DIR      "${SHADER_BUILD_DIR}"
)
slangc_compile(
    Shader_SDF2D
    SOURCE          "${CMAKE_CURRENT_LIST_DIR}/SDF2D.slang"
    OUTPUT_DIR      "${SHADER_BUILD_DIR}"
)
slangc_compile(
    Shader_MandelbrotCompute
    SOURCE          "${CMAKE_CURRENT_LIST_DIR}/MandelbrotCompute.slang"
    OUTPUT_DIR      "${SHADER_BUILD_DIR}"
)
