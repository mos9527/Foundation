cmake_minimum_required(VERSION 3.26)

include(FetchContent)
FetchContent_MakeAvailable(imgui)

add_library(ThirdParty_ImGui STATIC)
target_sources(ThirdParty_ImGui PRIVATE
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
    "Bindings/ImGui.cpp"
)
target_include_directories(ThirdParty_ImGui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    .
)
target_link_libraries(ThirdParty_ImGui PUBLIC
    Foundation_Native
    Foundation_RHIVulkan
    Foundation_RenderCore
)
