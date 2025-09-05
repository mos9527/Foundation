cmake_minimum_required(VERSION 3.26)

add_subdirectory(Shaders)

add_executable(Example_Triangle "Triangle.cpp")
target_link_libraries(Example_Triangle PUBLIC Foundation_Rendering)
add_dependencies(Example_Triangle Shader_Triangle)
