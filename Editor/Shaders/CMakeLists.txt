include(slangc_compile)

file(GLOB SLANG_FILES "${CMAKE_CURRENT_LIST_DIR}/*.slang")

add_custom_target(Editor_Shaders)
foreach(SlangFile IN LISTS SLANG_FILES)
    get_filename_component(ShaderName ${SlangFile} NAME_WE)
    
    # Skipping files starting with 'I' because apparently those are includes now.
    if(ShaderName MATCHES "^I")
        continue()
    endif()

    slangc_compile(
            "Shader_${ShaderName}"
            SOURCE          "${SlangFile}"
            OUTPUT_DIR      "${SHADER_BUILD_DIR}"
    )
    add_dependencies(Editor_Shaders "Shader_${ShaderName}")
endforeach()

