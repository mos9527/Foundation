include(slangc_compile)

file(GLOB SLANG_FILES "${CMAKE_CURRENT_LIST_DIR}/*.slang")

foreach(SlangFile IN LISTS SLANG_FILES)
    get_filename_component(ShaderName ${SlangFile} NAME_WE)
    slangc_compile(
            "Shader_${ShaderName}"
            SOURCE          "${SlangFile}"
            OUTPUT_DIR      "${SHADER_BUILD_DIR}"
            INCLUDE_DIRS     "${FOUNDATION_ROOT_DIR}/Source/Renderer/Shaders"
    )
endforeach()

