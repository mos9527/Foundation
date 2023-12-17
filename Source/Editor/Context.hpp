#pragma once
#include "../Common/FSM.hpp"
#include "../Runtime/RHI/RHI.hpp"
#include "Scene/Scene.hpp"
#include "Scene/SceneImporter.hpp"

/* RHI */
class LTCFittedLUT;
struct RHIContext {
    RHI::Device* device;
    RHI::RootSignature* rootSig;
    RHI::Swapchain* swapchain;

    static const UINT ROOTSIG_CB_EDITOR_GLOBAL = 0; // b0
    static const UINT ROOTSIG_CB_SHADER_GLOBAL = 1; // b1
    static const UINT ROOTSIG_CB_8_CONSTANTS = 2;   // b2
    static const UINT ROOTSIG_SAMPLER_TEXTURE = 3;
    static const UINT ROOTSIG_SAMPLER_DEPTH_COMP = 4;
    static const UINT ROOTSIG_SAMPLER_DEPTH_REDUCE = 5;

    // Const data resources
    LTCFittedLUT* data_LTCLUT;
    ~RHIContext() {
        delete device;
        delete rootSig;
        delete swapchain;
    }
};

/* EDITOR */
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
class MeshPicking;
struct EditorContext {
    EdtitorState state;    
    SceneImporter::SceneImporterAtomicStatus importStatus;
    entt::entity activeCamera = entt::tombstone;
    entt::entity editorHDRI = entt::tombstone;
    entt::entity editingComponent = entt::tombstone;
    struct {
        bool use = true;

        float diffuseIntensity = 1.0f;
        float specularIntensity = 1.0f;
        float occlusionStrength = 0.0f;

        float skyboxLod = 0.0f;
        float skyboxIntensity = 1.0f;
       
    } iblProbe;
    struct {
        float  edgeThreshold = 5.0f;
        float3 edgeColor = float3(232 / 255.0f, 125 / 255.0f, 13 / 255.0f);
    } pickerSilhouette;
    struct {
        MeshPicking* meshPicker;

        ViewportManipulationStates state;
        uint width = 1600;
        uint height = 1000;
    } viewport;
    struct {
        bool vsync = false;        
        uint width = 1600;
        uint height = 1000;
        bool wireframe = false;
        std::pair<RHI::Texture*, RHI::ShaderResourceView*> materialBuffer{};
        ImTextureID image = nullptr;
    } render;
    void set_viewport_dimension(uint width, uint height) {
        viewport.width = render.width = width;
        viewport.height = render.height = height;
    }
};
class SceneView;
struct SceneContext {
    Scene* scene;
    SceneView* views[RHI_DEFAULT_SWAPCHAIN_BACKBUFFER_COUNT];
};