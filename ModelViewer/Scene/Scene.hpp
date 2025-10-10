#pragma once
#include <Core/Pool.hpp>
#include <Rendering/GPUScene.hpp>
#include "Camera.hpp"
#include "Grid.hpp"
#include "Instance.hpp"
#include "Mesh.hpp"

namespace ModelViewer {
    using namespace Foundation;
    using namespace Rendering;
    using SceneHandle = uint32_t;
    constexpr uint32_t kSceneInvalid = ~0u;
    // TODO: Docs docs docs.
    //       Minus how everything here are still me figuring things out. Maybe later.
    class Scene
    {
        Allocator* mAllocator;
        GPUScene* mGPUScene;
        Pool<SceneHandle, MeshAllocation> mMeshes;
        VirtualAllocation mSceneParamsAllocation;
    public:
        Scene(GPUScene* scene, Allocator* allocator);

        float mTime{};
        
        /* -- Data -- */
        // TODO: Ugly - we do have a upper bound and data are in fact expected
        //       to be tightly packed. This works for now.
        uint32_t mInstanceCount{0};
        Camera mCamera{};
        Camera mCullingCamera{};
        Grid mGrid{};

        struct Params
        {
            Camera::Params camera;
            Camera::CullParams cullParams;
            uint32_t instanceCount;
        };
        void CommitParams();
        VirtualAllocation GetParamsAllocationRawOffset() const;

        SceneHandle PushMesh(MeshScratchBuffers const& data);
        MeshAllocation const& QueryMesh(SceneHandle handle);
        void FreeMesh(SceneHandle handle);
        
        Span<Instance> MapInstances();
        void UnmapInstances();
    };
}