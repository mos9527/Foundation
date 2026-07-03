cmake_minimum_required(VERSION 3.26)

add_library(ThirdParty_ImGui STATIC)
target_sources(ThirdParty_ImGui PRIVATE
    "imgui/imgui.cpp"
    "imgui/imgui_draw.cpp"
    "imgui/imgui_tables.cpp"
    "imgui/imgui_widgets.cpp"
    "imgui/imgui_demo.cpp"
    "imgui/backends/imgui_impl_sdl3.cpp"
    "imguizmo/ImGuizmo.cpp"
    "Bindings/ImGui.cpp"
    "Fonts/PlexSansIcon.c"
)
target_include_directories(ThirdParty_ImGui PUBLIC
    "imgui"
    "imgui/backends"
    "imguizmo"
    .
)

if (UNIX AND NOT APPLE)    
    target_include_directories(ThirdParty_ImGui PUBLIC ${X11_INCLUDE_DIR})
    target_link_libraries(ThirdParty_ImGui PUBLIC ${X11_LIBRARIES})
endif()

target_link_libraries(ThirdParty_ImGui PUBLIC
    Foundation_RenderCore
)

# Shader
include(slangc_compile)
slangc_compile(
    "Shader_ImGui"
    SOURCE          "${CMAKE_CURRENT_LIST_DIR}/Bindings/ImGui.slang"
    OUTPUT_DIR      "${SHADER_BUILD_DIR}"
    INCLUDE_DIRS    "${CMAKE_SOURCE_DIR}/Source/Renderer/Shaders"
)
add_dependencies(
    ThirdParty_ImGui
    Shader_ImGui
)