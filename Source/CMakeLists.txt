cmake_minimum_required(VERSION 3.26)

set(RUNTIME_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Runtime")
set(EDITOR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Editor")
set(COMMON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/Common")
set(PCH_FILE "${CMAKE_CURRENT_SOURCE_DIR}/pch")

add_compile_options(/Zc:__cplusplus /W3 /MP)
add_definitions(-DUNICODE)
add_definitions(-D_UNICODE)

file(GLOB DEPENDS_SOURCES_D3D "${DEPENDS}/D3D12MemoryAllocator/*.cpp" "${DEPENDS}/tracy/public/*.cpp")
file(GLOB DEPENDS_SOURCES_IMGUI "${DEPENDS}/imgui/imgui*.cpp" "${DEPENDS}/imgui/backends/imgui_impl_win32.cpp" "${DEPENDS}/imgui/backends/imgui_impl_dx12.cpp" "${DEPENDS}/ImGuizmo/*.cpp")

add_library(PCH STATIC "${PCH_FILE}.cpp" "${PCH_FILE}.hpp" ${DEPENDS_SOURCES_D3D} ${DEPENDS_SOURCES_IMGUI})
target_precompile_headers(PCH PUBLIC "${PCH_FILE}.hpp")

find_package(assimp CONFIG REQUIRED)
target_link_libraries(PCH PUBLIC assimp::assimp)

find_package(directx-headers CONFIG REQUIRED)
target_link_libraries(PCH PUBLIC Microsoft::DirectX-Headers)

find_package(directxmath CONFIG REQUIRED)
target_link_libraries(PCH PUBLIC Microsoft::DirectXMath)

find_package(directxtk CONFIG REQUIRED)
target_link_libraries(PCH PUBLIC Microsoft::DirectXTK)

find_package(glog CONFIG REQUIRED)
target_link_libraries(PCH PUBLIC glog::glog)

find_package(meshoptimizer CONFIG REQUIRED)
target_link_libraries(PCH PUBLIC meshoptimizer::meshoptimizer)

target_link_libraries(PCH PUBLIC "${DEPENDS}/winpixeventruntime/WinPixEventRuntime.lib")
target_link_libraries(PCH PUBLIC dxgi.lib)
target_link_libraries(PCH PUBLIC d3d12.lib)
target_link_libraries(PCH PUBLIC d3dcompiler.lib)
target_link_libraries(PCH PUBLIC dxcompiler.lib)

add_subdirectory(Runtime)
add_subdirectory(Editor)
add_subdirectory(Common)
