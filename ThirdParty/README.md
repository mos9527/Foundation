Thirdparty CMake files
---
`FetchContent` is used to manage most third-party dependencies. If something is _potentially_ used at all,
its `FetchContent_Declare` is in the `CMakeLists.txt` file here.

Note that the declarations here do not actually download or make the dependencies available. `FetchContent_MakeAvailable` is
only called in the `CMakeLists.txt` files where the dependencies are actually needed.

A few dependencies are vendored directly as source instead (e.g. `cgltf/`) - see their respective `README.md`/comments for why.

All rights to the respective third-party libraries are retained by their original authors.
