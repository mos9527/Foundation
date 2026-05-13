include(slangc_compile)

file(GLOB SLANG_FILES "${CMAKE_CURRENT_LIST_DIR}/*.slang")

add_custom_target(Editor_Shaders)
foreach(SlangFile IN LISTS SLANG_FILES)
    get_filename_component(ShaderName ${SlangFile} NAME_WE)
    
    # Skipping files starting with 'I' because apparently those are includes now.
    if(ShaderName MATCHES "^I")
        continue()
    endif()

    if(ShaderName STREQUAL "ERTPathTracer" OR ShaderName STREQUAL "EPSGBuffer")
        set(ShaderCapabilities
            SPV_GOOGLE_user_type
            spvDerivativeControl
            spvImageQuery
            spvImageGatherExtended
            spvSparseResidency
            spvMinLod
            spvFragmentFullyCoveredEXT
        )
    elseif(ShaderName STREQUAL "ECSLighting")
        set(ShaderCapabilities
            spvRayQueryKHR
        )
    elseif(ShaderName STREQUAL "ECSCullInstances" OR ShaderName STREQUAL "ECSCullMeshlets")
        set(ShaderCapabilities
            spvGroupNonUniformBallot
            spvGroupNonUniformArithmetic
        )
    else()
        set(ShaderCapabilities)
    endif()

    slangc_compile(
            "Shader_${ShaderName}"
            SOURCE          "${SlangFile}"
            OUTPUT_DIR      "${SHADER_BUILD_DIR}"
            CAPABILITIES    ${ShaderCapabilities}
    )
    add_dependencies(Editor_Shaders "Shader_${ShaderName}")

    # TODO - figure out a way for us to build shader variants _cleanly_
    if(ShaderName STREQUAL "ERTPathTracer")
        slangc_compile(
                "Shader_${ShaderName}_SER"
                SOURCE          "${SlangFile}"
                OUTPUT_DIR      "${SHADER_BUILD_DIR}"
                OUTPUT_NAME     "${ShaderName}_SER"
                DEFINES         PT_ENABLE_SER=1
                # Otherwise Slang emits VK_NV_ray_tracing_invocation_reorder. Eugh.
                CAPABILITIES    ${ShaderCapabilities}
                                spvShaderInvocationReorderEXT
                                spvShaderInvocationReorderNV
        )
        add_dependencies(Editor_Shaders "Shader_${ShaderName}_SER")
    endif()
endforeach()

