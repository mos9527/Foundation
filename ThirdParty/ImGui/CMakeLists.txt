cmake_minimum_required(VERSION 3.26)

include(FetchContent)
FetchContent_MakeAvailable(imgui)

add_library(ThirdParty_ImGui STATIC)
target_sources(ThirdParty_ImGui PRIVATE
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    "Bindings/ImGui.cpp"
    
)
target_include_directories(ThirdParty_ImGui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    .
)

if (UNIX AND NOT APPLE)
    # X11 for ImGui_ImplGlfw
    find_package(X11 REQUIRED)
    target_include_directories(ThirdParty_ImGui PUBLIC ${X11_INCLUDE_DIR})
    target_link_libraries(ThirdParty_ImGui PUBLIC ${X11_LIBRARIES})
endif()

target_link_libraries(ThirdParty_ImGui PUBLIC
    Foundation_Native
    Foundation_RenderCore
)
# Custom font
FetchContent_MakeAvailable(LxgwNeoXiHei)

# Shader
include(slangc_compile)
slangc_compile(
    "Shader_ImGui"
    SOURCE          "${CMAKE_CURRENT_LIST_DIR}/Bindings/ImGui.slang"
    OUTPUT_DIR      "${SHADER_BUILD_DIR}"
)
add_dependencies(
    ThirdParty_ImGui
    Shader_ImGui
)