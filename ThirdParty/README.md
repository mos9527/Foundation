Thirdparty CMake files
===
`FetchContent` is used to manage most third-party dependencies. If something is _potentially_ used at all,
its `FetchContent_Declare` is in the `CMakeLists.txt` file here.

Note that the declarations here do not actually download or make the dependencies available. `FetchContent_MakeAvailable` is
only called in the `CMakeLists.txt` files where the dependencies are actually needed.

A few dependencies are vendored directly as source instead — see notes below.

All rights to the respective third-party libraries are retained by their original authors.

Licenses
===

cgltf
---

Single-header glTF 2.0 parser. Vendored and substantially modified in-tree (Foundation glTF extensions /
typed fields); the upstream writer (`cgltf_write.h`) is not kept.

Upstream: https://github.com/jkuhlmann/cgltf

```
Copyright (c) 2018-2021 Johannes Kuhlmann

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
```
