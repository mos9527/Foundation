function(Foundation_DetectGitInfo)
    find_package(Git QUIET)

    set(FOUNDATION_GIT_COMMIT_HASH "unknown")
    set(FOUNDATION_GIT_COMMIT_SHORT "unknown")

    if(GIT_FOUND)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} rev-parse HEAD
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
            OUTPUT_VARIABLE FOUNDATION_GIT_COMMIT_HASH
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE GIT_HASH_RESULT
        )
        execute_process(
            COMMAND ${GIT_EXECUTABLE} rev-parse --short HEAD
            WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}
            OUTPUT_VARIABLE FOUNDATION_GIT_COMMIT_SHORT
            OUTPUT_STRIP_TRAILING_WHITESPACE
            RESULT_VARIABLE GIT_SHORT_HASH_RESULT
        )

        if(NOT GIT_HASH_RESULT EQUAL 0)
            set(FOUNDATION_GIT_COMMIT_HASH "unknown")
        endif()
        if(NOT GIT_SHORT_HASH_RESULT EQUAL 0)
            set(FOUNDATION_GIT_COMMIT_SHORT "unknown")
        endif()
    endif()

    set(FOUNDATION_GIT_COMMIT_HASH "${FOUNDATION_GIT_COMMIT_HASH}" PARENT_SCOPE)
    set(FOUNDATION_GIT_COMMIT_SHORT "${FOUNDATION_GIT_COMMIT_SHORT}" PARENT_SCOPE)
endfunction()
