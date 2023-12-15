cmake_minimum_required(VERSION 3.26)
project(Foundation)

set(CMAKE_CXX_STANDARD 20)
set(DEPENDS "${CMAKE_SOURCE_DIR}/Dependencies")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/Bin")

add_compile_definitions(SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/Source")
include_directories("${VCPKG_INSTALLED_DIR}/x64-windows/include")
include_directories("${DEPENDS}/D3D12MemoryAllocator")
include_directories("${DEPENDS}/entt/single_include")
include_directories("${DEPENDS}/winpixeventruntime/Include")
include_directories("${DEPENDS}/imgui")
include_directories("${DEPENDS}/tracy/public")

add_subdirectory(Source)
