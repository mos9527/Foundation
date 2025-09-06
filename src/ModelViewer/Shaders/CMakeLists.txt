include(slangc_compile)
slangc_compile(
    Shader_Mesh
    SOURCE          "${CMAKE_CURRENT_LIST_DIR}/Mesh.slang"
    OUTPUT_DIR      "${SHADER_BUILD_DIR}"
)
