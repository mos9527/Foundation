include(slangc_compile)

file(GLOB SLANG_FILES "${CMAKE_CURRENT_LIST_DIR}/*.slang")

add_custom_target(Editor_Shaders)
foreach(SlangFile IN LISTS SLANG_FILES)
    get_filename_component(ShaderName ${SlangFile} NAME_WE)
    slangc_compile(
            "Shader_Editor_${ShaderName}"
            SOURCE          "${SlangFile}"
            OUTPUT_DIR      "${SHADER_BUILD_DIR}/Editor"
    )
    add_dependencies(Editor_Shaders "Shader_Editor_${ShaderName}")
endforeach()
