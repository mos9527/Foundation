
file(GLOB SAMPLE_FILES "${CMAKE_CURRENT_LIST_DIR}/*.jpg")

set(OUTPUT_FILES "")
foreach (FILE ${SAMPLE_FILES})
    get_filename_component(MODEL_NAME ${FILE} NAME)
    set(OUTPUT_FILE "${ASSET_BUILD_DIR}/${FILE_NAME}")
    add_custom_command(
        OUTPUT  "${OUTPUT_FILE}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${FILE}" "${OUTPUT_FILE}"
        DEPENDS "${FILE}"
        COMMENT "Copying sample file: ${FILE_NAME}"
        VERBATIM
    )
    list(APPEND OUTPUT_FILES "${OUTPUT_FILE}")
endforeach ()
add_custom_target(Examples_SampleFiles ALL DEPENDS ${OUTPUT_FILES})
