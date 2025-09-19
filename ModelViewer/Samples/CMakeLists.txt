
file(GLOB SAMPLE_MODELS "${CMAKE_CURRENT_LIST_DIR}/*.obj")

set(OUTPUT_FILES "")
foreach (MODEL ${SAMPLE_MODELS})
    get_filename_component(MODEL_NAME ${MODEL} NAME)
    set(OUTPUT_FILE "${ASSET_BUILD_DIR}/${MODEL_NAME}")
    add_custom_command(
        OUTPUT  "${OUTPUT_FILE}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${MODEL}" "${OUTPUT_FILE}"
        DEPENDS "${MODEL}"
        COMMENT "Copying sample model: ${MODEL_NAME}"
        VERBATIM
    )
    list(APPEND OUTPUT_FILES "${OUTPUT_FILE}")
endforeach ()
add_custom_target(SampleModels ALL DEPENDS ${OUTPUT_FILES})
