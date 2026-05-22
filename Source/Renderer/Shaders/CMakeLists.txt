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
        if(ShaderName STREQUAL "ERTPathTracer")
            list(APPEND ShaderCapabilities
                # Otherwise Slang emits VK_NV_ray_tracing_invocation_reorder. Eugh.
                spvShaderInvocationReorderEXT
            )
        endif()
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
endforeach()
