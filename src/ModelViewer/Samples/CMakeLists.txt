include(FetchContent)
FetchContent_Declare(
    kitten.obj
    URL https://raw.githubusercontent.com/zeux/niagara/refs/heads/master/data/kitten.obj
    SOURCE_DIR "${ASSET_BUILD_DIR}"
    DOWNLOAD_NO_EXTRACT TRUE
)
FetchContent_MakeAvailable(kitten.obj)
