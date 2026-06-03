include(slangc_compile)

file(GLOB SLANG_FILES "${CMAKE_CURRENT_LIST_DIR}/*.slang")

add_custom_target(Foundation_Shaders)
foreach(SlangFile IN LISTS SLANG_FILES)
    get_filename_component(ShaderName ${SlangFile} NAME_WE)
    # Skipping files starting with 'I' because apparently those are includes now
    # .slangi sounds even worse. I'll keep it this way then.
    if(ShaderName MATCHES "^I")
        continue()
    endif()
    set(ShaderCapabilities
            SPV_GOOGLE_user_type
            spvDerivativeControl
            spvImageQuery
            spvImageGatherExtended
            spvSparseResidency
            spvMinLod
            spvFragmentFullyCoveredEXT
            spvGroupNonUniformBallot
            spvGroupNonUniformArithmetic
            spvRayQueryKHR
            spvShaderInvocationReorderEXT
    )
    slangc_compile(
            "Shader_${ShaderName}"
            SOURCE          "${SlangFile}"
            OUTPUT_DIR      "${SHADER_BUILD_DIR}"
            CAPABILITIES    ${ShaderCapabilities}
    )
    # Variants (Yes, in CMake.)
    # SER is a special case since it's always there in the IR - and some drivers (Lavapipe) doesn't understand it.
    # Avoid emitting them in the first place with a macro guard.
    if (ShaderName STREQUAL "ERTPathTracer")
        slangc_compile(
                "Shader_${ShaderName}_SER"
                SOURCE          "${SlangFile}"
                OUTPUT_DIR      "${SHADER_BUILD_DIR}"
                OUTPUT_NAME     "${ShaderName}_SER"
                CAPABILITIES    ${ShaderCapabilities} spvShaderInvocationReorderEXT
                DEFINES         "USE_SER=1"
        )
        add_dependencies(Foundation_Shaders "Shader_${ShaderName}_SER")
    endif()
    add_dependencies(Foundation_Shaders "Shader_${ShaderName}")
endforeach()
