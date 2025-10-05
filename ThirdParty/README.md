Thirdparty CMake files
---
`FetchContent` is used exclusively to manage third-party dependencies. And if something is _potentially_ used at all,
its `FetchContent_Declare` is in the `CMakeLists.txt` file here.

Note that the declarations here do not actually download or make the dependencies available. `FetchContent_MakeAvailable` is
only called in the `CMakeLists.txt` files where the dependencies are actually needed.

All rights to the respective third-party libraries are retained by their original authors.
