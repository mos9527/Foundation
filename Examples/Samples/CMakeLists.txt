file(
        GLOB
        SAMPLE_FILES
        "${CMAKE_CURRENT_LIST_DIR}/*.jpg"
        "${CMAKE_CURRENT_LIST_DIR}/*.obj"
)

add_custom_target(Examples_SampleFiles)
foreach (FILE ${SAMPLE_FILES})
    get_filename_component(MODEL_NAME ${FILE} NAME)
    set(OUTPUT_FILE "${ASSET_BUILD_DIR}/${FILE_NAME}")
    add_custom_command(
        TARGET Examples_SampleFiles POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${FILE}" "${OUTPUT_FILE}"
        COMMENT "Copying sample file: ${FILE_NAME} -> ${OUTPUT_FILE}"
        VERBATIM
    )
endforeach ()

