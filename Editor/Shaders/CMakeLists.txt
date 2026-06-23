include(slangc_compile)

file(GLOB SLANG_FILES "${CMAKE_CURRENT_LIST_DIR}/*.slang")
set(ShaderCapabilities
        SPV_GOOGLE_user_type
        spvImageQuery
        spvImageGatherExtended
        spvSparseResidency
        spvMinLod
        spvFragmentFullyCoveredEXT
        spvGroupNonUniformBallot
        spvGroupNonUniformArithmetic
)
add_custom_target(Editor_Shaders)
foreach(SlangFile IN LISTS SLANG_FILES)
    get_filename_component(ShaderName ${SlangFile} NAME_WE)
    slangc_compile(
            "Shader_Editor_${ShaderName}"
            SOURCE          "${SlangFile}"
            OUTPUT_DIR      "${SHADER_BUILD_DIR}/Editor"
            INCLUDE_DIRS    "${CMAKE_SOURCE_DIR}/Source/Renderer/Shaders"
            CAPABILITIES    ${ShaderCapabilities}
    )
    add_dependencies(Editor_Shaders "Shader_Editor_${ShaderName}")
endforeach()
