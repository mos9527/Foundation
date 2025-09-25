include(slangc_compile)

file(GLOB SLANG_FILES "${CMAKE_CURRENT_LIST_DIR}/*.slang")
set(SHADER_TARGETS)

foreach(SlangFile IN LISTS SLANG_FILES)
    get_filename_component(ShaderName "${SlangFile}" NAME_WE)
    set(PerShaderTarget "Shader_${ShaderName}")
    slangc_compile(
            "${PerShaderTarget}"
            SOURCE      "${SlangFile}"
            OUTPUT_DIR  "${SHADER_BUILD_DIR}"
    )
    list(APPEND SHADER_TARGETS "${PerShaderTarget}")
endforeach()

add_custom_target(ModelViewer_Shaders ALL DEPENDS ${SHADER_TARGETS})