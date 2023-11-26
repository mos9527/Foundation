#pragma once
#include "../Common/FSM.hpp"
#include "../Common/Task.hpp"
#include "../Runtime/RHI/RHI.hpp"
#include "../Runtime/Asset/AssetRegistry.hpp"
#include "Scene/Scene.hpp"
#include "Scene/SceneView.hpp"
#include "Scene/SceneGraph.hpp"
#include "Renderer/Deferred.hpp"
#include "Scene/SceneImporter.hpp"
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
    std::mutex renderMutex;
    std::mutex loadMutex;
};
    
struct EditorContext {
    EdtitorState state;
    SceneImporter::SceneImporterAtomicStatus importStatus;
};
    
struct ViewportContext {
    RHI::ShaderResourceView* frame = nullptr;
    entt::entity camera;
    bool vsync = false;
    uint frameFlags = 0;
    uint width = 0;
    uint height = 0;
};
