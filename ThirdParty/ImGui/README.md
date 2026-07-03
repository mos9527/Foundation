ImGui Binding
---
[Dear ImGui](https://github.com/ocornut/imgui) binding for Foundation.

See [Examples/ImGui](https://github.com/mos9527/Foundation/blob/vulkan/Examples/ImGui.cpp) and
[ModelViewer's Render](https://github.com/mos9527/Foundation/blob/vulkan/ModelViewer/Render.cpp) for usage examples.

Vendored sources
---
Unlike the rest of `ThirdParty`, ImGui and ImGuizmo are vendored directly (not `FetchContent`) since they're
lightly patched/pinned and rarely change:

- `imgui/` - [ocornut/imgui](https://github.com/ocornut/imgui) `v1.92.7-docking` (core + `backends/imgui_impl_sdl3`)
- `imguizmo/` - [hrydgard/ImGuizmo](https://github.com/hrydgard/ImGuizmo) `5088891e308f14556c6c961aac1be79e1fc1850f`

To update, replace the files in these directories with the new upstream versions and bump the versions noted above.