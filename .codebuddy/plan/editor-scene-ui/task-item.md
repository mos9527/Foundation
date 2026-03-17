# Implementation Plan — Editor Scene UI Refactor

- [ ] 1. Enable ImGui Docking and create DockSpace shell
   - In `SDLMain.cpp`, after `ImGui_ImplFoundation_SetupContextWithDefaultStyles()`, set `ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable`
   - In `Editor.cpp`, add a new function `EditorDockSpaceAndMenuBar()` that calls `ImGui::DockSpaceOverViewport()` and `ImGui::BeginMainMenuBar()` (empty menus for now)
   - Call `EditorDockSpaceAndMenuBar()` at the top of `FRunning()` (before `FRunningImGui()`), and also in `FInit()` so the UI is visible before a scene is loaded
   - Remove the `enableImGui` toggle and `TAB` keybind — ImGui is now always-on
   - Wrap existing Camera, Rendering, Profiler panels inside named `ImGui::Begin("Camera")` etc. windows (already the case — no change needed, they will auto-dock)
   - _Requirements: 1.1, 1.2, 1.3, 1.5_

- [ ] 2. Remove mandatory CLI scene argument; support empty-scene startup
   - In `EditorOnFrame`, change `FEInitEnter` to check `GContext->args.size()`: if no scene path, skip `FInitEnter()` and go directly to `FEInit` with empty global vectors (`GSInstances`, `GSMeshes`, etc.)
   - In `FInitEnter`, guard the `LoadScene` call behind `args.size() == 2`
   - In `FRunningEnter` / `RendererSetup` / `PathTracerSetup`, handle the case where `GSInstances` is empty (zero instances, no TLAS) — check `GetTLAS()` returns `nullptr` and skip RT dispatch
   - Verify that `GPUScene::BuildTLAS` gracefully handles an empty instance span (add early return if `instances.empty()`)
   - _Requirements: 6.1, 6.2, 6.3_

- [ ] 3. Add persistent `FScene` to retain CPU-side data across the session
   - Promote the local `FScene scene(GLOBAL_ALLOC)` in `FInitEnter` to a file-scope `static FScene GPersistentScene(GLOBAL_ALLOC)`
   - After initial load and GPU upload, keep `GPersistentScene` alive (do not destroy)
   - When additive loading appends data, append to `GPersistentScene` as well (so it always mirrors the full CPU state)
   - Add a `static String GCurrentSavePath` to track the last saved/loaded file path
   - _Requirements: 5.3, 5.4_

- [ ] 4. Implement Win32 native file dialog helpers
   - Create a new file `Editor/FileDialog.hpp` with `#ifdef _WIN32` guards
   - Implement `Optional<String> OpenFileDialog(const wchar_t* filter, const wchar_t* title)` using `GetOpenFileNameW` from `<commdlg.h>`
   - Implement `Optional<String> SaveFileDialog(const wchar_t* filter, const wchar_t* title, const wchar_t* defaultExt)` using `GetSaveFileNameW`
   - In `CMakeLists.txt`, add `target_link_libraries(Editor PRIVATE ... comdlg32)` (or rely on Windows SDK auto-link)
   - _Requirements: 2.2, 2.6, 2.8_

- [ ] 5. Implement additive scene loading (`AppendScene`)
   - In `Editor.cpp`, add a function `void AppendScene(StringView path)` that:
     1. Creates a temporary `FScene newScene(GLOBAL_ALLOC)` and calls `LoadScene(path, newScene)`
     2. Records current counts: `meshBase = GPersistentScene.mMeshes.size()`, `texBase = GPersistentScene.mTextures.size()`, `matBase = GPersistentScene.mMaterials.size()`
     3. Appends `newScene.mMeshes` and `newScene.mTextures` to `GPersistentScene` and uploads them to `GPUScene` via `ImmediateUpload` (building a `textureIDMap` and `meshOffsets` vector for the new data)
     4. Remaps `newScene.mMaterials` texture indices by adding `texBase`, appends to `GPersistentScene.mMaterials`, converts to `GSMaterial` and appends to `GSMaterials`
     5. Remaps `newScene.mInstances` mesh/material indices by adding `meshBase`/`matBase`, appends to `GPersistentScene.mInstances`, converts to `GSInstance` (using new `meshOffsets`) and appends to `GSInstances`
     6. Re-uploads full `GSInstances` and `GSMaterials` arrays to GPU ring buffers
     7. Builds BLAS for the new meshes and rebuilds TLAS for the full instance set
     8. If this is the first load (no prior data), also takes camera and light data from `newScene`
     9. Sets `FEState = FERunningEnter` to re-setup the renderer
   - Handle load failure with try/catch or error check — log and return without modifying state
   - _Requirements: 3.1, 3.2, 3.3, 3.4, 3.5, 3.6, 3.7_

- [ ] 6. Implement scene saving (`SaveScene`)
   - In `Editor.cpp`, add a function `void SaveScene(StringView path)` that:
     1. Opens a `FileWriter(path)` on the persistent `GPersistentScene`
     2. Calls `FSerialize(writer, GPersistentScene)`
     3. Updates `GCurrentSavePath`
     4. Logs success or error
   - _Requirements: 5.1, 5.2, 5.4, 5.5_

- [ ] 7. Wire up the Main Menu Bar with File operations
   - In `EditorDockSpaceAndMenuBar()`, flesh out the menu bar:
     - "File > Open…" → call `OpenFileDialog` with filter `"Scene Files\0*.gltf;*.glb;*.fscn\0"`, on success call `AppendScene(path)`
     - "File > Save" → if `GCurrentSavePath` is set, call `SaveScene(GCurrentSavePath)`, else fall through to Save As
     - "File > Save As…" → call `SaveFileDialog` with `.fscn` filter, on success call `SaveScene(path)`
   - _Requirements: 2.1, 2.2, 2.3, 2.4, 2.5, 2.6, 2.7, 2.8_

- [ ] 8. Implement Scene Hierarchy panel
   - In `Editor.cpp`, add a new function `void FHierarchyPanel()` that:
     1. `ImGui::Begin("Hierarchy")` — a dockable window
     2. If `GSInstances.empty()`, display "No instances loaded"
     3. Otherwise, iterate `GSInstances` and render each as a `Selectable` with label `"Instance %d — Mesh %d, Mat %d"`
     4. On click, store the selected index in a `static int GSelectedInstance = -1`
   - Add a simple "Inspector" section (inside the same window or a separate `ImGui::Begin("Inspector")`):
     1. If `GSelectedInstance >= 0`, display the instance's transform (position, rotation, scale), `meshOffset`, `materialIndex` as read-only text
   - Call `FHierarchyPanel()` from inside `FRunningImGui()` (and also from `FInit()` so it shows when no renderer is active)
   - _Requirements: 4.1, 4.2, 4.3, 4.4, 4.5_

- [ ] 9. Setup default docking layout on first launch
   - In `EditorDockSpaceAndMenuBar()`, after `DockSpaceOverViewport()`, check if this is the first frame (no `imgui.ini` present or a `static bool firstTime` flag)
   - If first launch, use `ImGui::DockBuilderSplitNode` to programmatically create a default layout: Hierarchy (left ~20%), Viewport (center), Camera+Rendering+Profiler (right or bottom ~25%)
   - On subsequent launches, ImGui's `imgui.ini` persistence handles layout automatically
   - _Requirements: 1.3, 1.4_

- [ ] 10. Refactor `FInit` state to show UI before scene is loaded
   - Currently `FInit()` immediately transitions to `FERunningEnter`. Change it to enter a loop that: renders the DockSpace + menu bar + hierarchy (empty), processes events, and only transitions to `FERunningEnter` when a scene has been loaded (or stays in `FEInit` for an empty scene)
   - Ensure `RendererSetupImGuiOnly` is called so that ImGui renders even without a full renderer pipeline
   - This allows the user to use "File > Open" before any scene is loaded
   - _Requirements: 1.5, 6.1_
