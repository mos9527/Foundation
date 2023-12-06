#pragma once
#include "../Common/FSM.hpp"
#include "../Common/Task.hpp"
#include "../Runtime/RHI/RHI.hpp"
#include "../Runtime/Asset/AssetRegistry.hpp"
#include "Scene/Scene.hpp"
#include "Scene/SceneView.hpp"
#include "Scene/SceneGraph.hpp"
#include "Scene/SceneImporter.hpp"
#include "Renderer/Deferred.hpp"
#include "Processor/IBLProbeProcessor.hpp"
#include "Processor/LTCTableProcessor.hpp"
#include "Processor/MeshSelectionProcessor.hpp"
#include "Processor/StaticMeshBufferProcessor.hpp"
#include "Processor/SkinnedMeshBufferProcessor.hpp"
enum class EditorEvents {
    OnSetup,
    OnLoad,
    OnLoadComplete,
    OnRunFrame,
    OnDestroy
};

enum class EditorStates {
    Uninitialized,
    Setup,
    Loading,
    Running,
    Destroyed
};

struct EdtitorState : public FSM::EFSM<EditorStates, EditorEvents, EditorStates::Uninitialized> {
public:
    void fail(EditorEvents event) { 
        /* do nothing. state is simply not transitioned. */ 
    }
    EditorStates transition(EditorEvents event) {
        using enum EditorStates;
        using enum EditorEvents;
        switch (event)
        {
        case OnSetup:
            if (state == Uninitialized) state = Setup;
            else fail(event);
            break;
        case OnLoad:
            if (state == Running || state == Setup) state = Loading;
            else fail(event);
            break;
        case OnLoadComplete:
            if (state == Loading) state = Running;
            else fail(event);
            break;
        case OnRunFrame:
            if (state != Loading && state != Destroyed) state = Running;
            else fail(event);
            break;
        case OnDestroy:
            if (state == Running) state = Destroyed;
            else fail(event);
            break;
        default:
            fail(event);
            break;
        }
        return state;
    }
};

struct SceneContext {
    Scene* scene;
    SceneView* views[RHI_DEFAULT_SWAPCHAIN_BACKBUFFER_COUNT];
};

struct RenderContext {
    DeferredRenderer* renderer;
    TracyLockable(std::mutex, renderMutex);
    TracyLockable(std::mutex, loadMutex);    
};
    
struct EditorContext {
    EdtitorState state;
    SceneImporter::SceneImporterAtomicStatus importStatus;
    /* DATA / PROCESSERS */
    IBLProbeProcessor* iblProbe;
    struct {
        bool use = true;

        float diffuseIntensity = 1.0f;
        float specularIntensity = 1.0f;
        float occlusionStrength = 0.0f;

        float skyboxLod = 0.0f;
        float skyboxIntensity = 1.0f;
    } iblProbeParam;
    LTCTableProcessor* ltcTable;
    struct {
        float  edgeThreshold = 5.0f;
        float3 edgeColor = float3(232 / 255.0f, 125 / 255.0f, 13 / 255.0f);
    } silhouetteParam;

    entt::entity editingComponent = entt::tombstone;
    MeshSelectionProcessor* meshSelection;
};
    

enum class ViewportManipulationEvents {
    HoverWithoutGizmo,
    HoverWithGizmo,
    MouseDown,
    MouseRelease,
    MouseLeave,
    ShiftDown,
    ShiftRelease
};
enum class ViewportManipulationState {
    Nothing,
    HoverOnCamera,
    HoverOnGizmo,
    UsingCamera,
    UsingCameraOffsetView,
    UsingGizmo
};

struct ViewportManipulationStates : public FSM::EFSM<ViewportManipulationState, ViewportManipulationEvents, ViewportManipulationState::Nothing> {
public:
    void fail(ViewportManipulationEvents event) {
        /* do nothing. state is simply not transitioned. */
    }
    ViewportManipulationState transition(ViewportManipulationEvents event) {
        using enum ViewportManipulationState;
        using enum ViewportManipulationEvents;
        switch (event)
        {
        case HoverWithoutGizmo:
            if (state == Nothing) state = HoverOnCamera;
            else fail(event);
            break;
        case HoverWithGizmo:
            if (state == Nothing) state = HoverOnGizmo;
            else fail(event);
            break;
        case MouseDown:
            if (state == HoverOnCamera) state = UsingCamera;
            else if (state == HoverOnGizmo) state = UsingGizmo;
            else fail(event);
            break;
        case ShiftDown:
            if (state == UsingCamera) state = UsingCameraOffsetView;
            else fail(event);
            break;
        case ShiftRelease:
            if (state == UsingCameraOffsetView) state = UsingCamera;
            else fail(event);
            break;
        case MouseRelease:
        case MouseLeave:
            state = Nothing;
            break;
        default:
            fail(event);
        }
        return state;
    }
};

struct ViewportContext {
    ViewportManipulationStates state;
    entt::entity camera;
    bool vsync = false;
    uint frameFlags = 0;
    uint width = 0;
    uint height = 0;
};
