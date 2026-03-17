# Requirements Document — Editor Scene UI Refactor

## Introduction

The Editor currently operates with a hard-coded, single-scene-load workflow driven by command-line arguments. All ImGui panels (Camera, Rendering, Profiler) are free-floating and toggled on/off with a single `TAB` key. There is no scene management UI, no ability to load additional assets at runtime, no hierarchy view, and no scene saving.

This refactor introduces a **basic but functional** editor UI shell:

1. **Docked ImGui layout** — all panels organized via ImGui's built-in DockSpace (v1.92.3-docking already fetched).
2. **Additive scene loading** — open file dialog (Win32 `GetOpenFileName` / `IFileOpenDialog`) to load `.gltf`/`.glb`/`.fscn` files *additively* into the existing scene, uploading new data to `GPUScene` and rebuilding acceleration structures.
3. **Scene hierarchy view** — a simple tree/list showing loaded instances, their mesh & material indices, and allowing selection (no transform gizmos in V1).
4. **Scene saving** — serialize the current composite `FScene` to `.fscn` using the existing `FSerialize` infrastructure.

**Scope guard — keep it simple (V1):**
- Windows only for file dialogs.
- No undo/redo.
- No drag-and-drop reordering in the hierarchy.
- No per-instance property editing (beyond selection highlight).
- No transform gizmos.

---

## Requirements

### Requirement 1 — Docked Layout

**User Story:** As an editor user, I want all ImGui panels to be docked in a stable, resizable layout, so that I can see the viewport, hierarchy, and properties simultaneously without floating windows.

#### Acceptance Criteria

1. WHEN the editor starts THEN the system SHALL enable `ImGuiConfigFlags_DockingEnable` on the ImGui IO config.
2. WHEN `FRunningImGui()` is called THEN the system SHALL create a full-window `DockSpace` covering the main window area, using `ImGui::DockSpaceOverViewport()`.
3. IF the user has not customized the layout THEN the system SHALL provide a default initial layout (e.g., hierarchy on the left, viewport center, profiler/camera on the right or bottom).
4. WHEN the user rearranges docked panels THEN ImGui SHALL persist the layout across frames (built-in behavior; `imgui.ini`).
5. WHEN the editor is in the `FEInit` state (before a scene is loaded into the renderer) THEN the system SHALL still show the docked UI with the menu bar and hierarchy (empty).

### Requirement 2 — Main Menu Bar with File Operations

**User Story:** As an editor user, I want a top-level menu bar with File > Open, File > Save, and File > Save As actions, so that I can manage scene files without relying on CLI arguments.

#### Acceptance Criteria

1. WHEN `FRunningImGui()` is called THEN the system SHALL render an `ImGui::BeginMainMenuBar()` containing at least a "File" menu.
2. WHEN the user selects "File > Open…" THEN the system SHALL invoke a native Windows file-open dialog filtered to `*.gltf;*.glb;*.fscn`.
3. WHEN the user selects a file from the dialog THEN the system SHALL load the scene **additively** (see Req 3).
4. WHEN the user selects "File > Save" AND a save path is known THEN the system SHALL serialize the current scene to that path as `.fscn`.
5. WHEN the user selects "File > Save" AND no save path is known THEN the system SHALL behave as "Save As…".
6. WHEN the user selects "File > Save As…" THEN the system SHALL invoke a native Windows file-save dialog filtered to `*.fscn`, then serialize.
7. IF serialization succeeds THEN the system SHALL log a success message.
8. IF the file dialog is cancelled THEN the system SHALL take no action.

### Requirement 3 — Additive Scene Loading

**User Story:** As an editor user, I want to load additional scene files into the current scene without replacing existing content, so that I can compose complex scenes from multiple assets.

#### Acceptance Criteria

1. WHEN a new scene file is loaded THEN the system SHALL append its meshes, textures, materials, instances, cameras, and lights to the existing `FScene` data, applying correct index remapping (mesh indices, texture indices, material indices offset by the current counts).
2. WHEN new meshes/textures are appended THEN the system SHALL upload them to `GPUScene` via `ImmediateUpload`.
3. WHEN new instances/materials are appended THEN the system SHALL re-upload the full instance and material arrays to `GPUScene` ring buffers and update `UBO` offsets.
4. WHEN new meshes are appended THEN the system SHALL build BLAS for the new meshes and rebuild (or update) the TLAS.
5. WHEN GPU data upload is complete THEN the system SHALL transition to `FERunningEnter` to re-setup the renderer.
6. IF loading fails (file not found, parse error) THEN the system SHALL log an error and leave the existing scene unchanged.
7. WHEN the first scene is loaded (no prior data) THEN the system SHALL also accept camera and light data from the file to initialize the view.

### Requirement 4 — Scene Hierarchy Panel

**User Story:** As an editor user, I want to see a list of all instances in the current scene, so that I can understand what is loaded and select objects.

#### Acceptance Criteria

1. WHEN the hierarchy panel is visible THEN the system SHALL display a scrollable list/tree of all `GSInstance` entries, each labeled with its index and mesh/material info (e.g., "Instance 0 — Mesh 3, Mat 1").
2. WHEN the user clicks an entry in the hierarchy THEN the system SHALL mark it as "selected" and store the selected index.
3. IF an instance is selected THEN the system SHALL display its basic properties (transform, meshOffset, materialIndex) in a read-only "Inspector" section below or beside the hierarchy.
4. WHEN the scene changes (additive load) THEN the hierarchy list SHALL refresh to reflect the new set of instances.
5. IF the scene is empty THEN the hierarchy panel SHALL display "No instances loaded".

### Requirement 5 — Scene Saving

**User Story:** As an editor user, I want to save the current composite scene (including all additively-loaded data) to a `.fscn` file, so that I can reload it later without re-composing.

#### Acceptance Criteria

1. WHEN save is triggered THEN the system SHALL reconstruct an `FScene` from the current editor state (`GSInstances`, `GSMaterials`, and the retained CPU-side mesh/texture data).
2. WHEN the `FScene` is constructed THEN the system SHALL call `FSerialize(FileWriter(...), scene)` to write it to disk.
3. IF the editor does not retain CPU-side mesh/texture data (current architecture discards `FScene` after GPU upload) THEN the system SHALL be modified to retain a persistent `FScene` (or equivalent) for the editor session's lifetime.
4. WHEN save completes THEN the system SHALL update the known save path so subsequent "File > Save" writes to the same file.
5. IF writing fails (disk error) THEN the system SHALL log an error.

### Requirement 6 — Remove Mandatory CLI Argument

**User Story:** As an editor user, I want to launch the editor without specifying a scene file on the command line, so that I can open scenes from the UI.

#### Acceptance Criteria

1. WHEN the editor starts with no scene path argument THEN the system SHALL skip `FInitEnter`'s scene load and proceed to `FEInit` → `FERunningEnter` with an empty scene.
2. WHEN the editor starts with a scene path argument THEN the system SHALL load that scene as before (backward compatible).
3. WHEN an empty scene is active THEN the renderer setup SHALL still succeed (zero instances, no TLAS).

---

## Technical Notes

- **ImGui Docking**: v1.92.3-docking is already fetched. Enable `ImGuiConfigFlags_DockingEnable` in `ImGui_ImplFoundation_SetupContextWithDefaultStyles()` or in `SDLMain.cpp` after setup.
- **Win32 File Dialog**: Use `<windows.h>` + `GetOpenFileNameW` / `GetSaveFileNameW` (or COM `IFileOpenDialog`). Keep behind `#ifdef _WIN32`. Link against `comdlg32`.
- **Persistent FScene**: Currently `FScene scene(GLOBAL_ALLOC)` is a local in `FInitEnter` and destroyed after GPU upload. Must promote to static/global lifetime.
- **Index Remapping**: When appending scene B to scene A, all of B's material indices, texture references inside materials, and mesh indices in instances must be offset by A's current counts.
- **Serialization**: `FSerialize<FScene>` and `FileWriter` already exist — no new serialization code needed beyond a `SaveScene()` wrapper function.
