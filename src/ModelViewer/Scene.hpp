#pragma once
#include "Mesh.hpp"
#include "SceneData.hpp"
namespace Foundation {
    using namespace Foundation::RHI;
    using namespace Foundation::Core;
    using namespace Foundation::Rendering;
    using namespace Foundation::Math;
    #include "Shaders/Common.h"

    /**
     * @brief Scene representation for GPU driven rendering.
     *
     * Primitive data, instance data, and global data are represented in flat buffers.
     * See @ref PrimitiveMetadata, @ref InstanceMetadata, @ref GlobalMetadata for data layouts.
     */
    class Scene {
        Allocator* m_allocator{ nullptr };
        SceneData m_data;

        bool m_dirty = true;
        bool HasUpdates() const { return m_dirty || m_data.HasUpdates(); }
        void Update(RHICommandList* cmd);
    public:
        Scene(Allocator* allocator, RHIDevice* device, SceneDataDesc const& desc);

        SceneHandle AddMesh(Span<const Vertex> vertices, Span<const Index> indices, SceneHandle& outVtx, SceneHandle& outIdx);
        void FreeMesh(SceneHandle mesh, SceneHandle vtx, SceneHandle idx);

        SceneHandle AddInstance(InstanceMetadata data);
        void UpdateInstance(SceneHandle instance, InstanceMetadata const& data);
        void FreeInstance(SceneHandle handle);

        auto* CreateUpdatePass(
            Renderer* renderer,
            ResourceHandle& outInstance,
            ResourceHandle& outPrimitive,
            ResourceHandle& outVertex,
            ResourceHandle& outIndex
        ) {
            auto& data = m_data;
            ResourceHandle Instance  = createResource(renderer, "Scene Instances", data.GetInstanceDataBuffer());
            ResourceHandle Primitive = createResource(renderer, "Scene Primitives", data.GetPrimitiveDataBuffer());
            ResourceHandle Vertex    = createResource(renderer, "Scene Flat Vertices", data.GetVertexDataBuffer());
            ResourceHandle Index     = createResource(renderer, "Scene Flat Indices", data.GetIndexDataBuffer());
            outInstance = Instance, outPrimitive = Primitive, outVertex = Vertex, outIndex = Index;
            return createPass(
                renderer, "Scene Update",
                RHIDeviceQueueType::Graphics,
                [=, this](PassHandle self, Renderer* r) {
                    r->BindBufferCopyDst(self, Instance);
                    r->BindBufferCopyDst(self, Primitive);
                    r->BindBufferCopyDst(self, Vertex);
                    r->BindBufferCopyDst(self, Index);
                },
                [=, this](PassHandle self, Renderer* r, RHICommandList* cmd) {
                    Update(cmd);
                },
                [=, this](PassHandle self, Renderer* r)
                {
                    return HasUpdates();
                }
            );
        }
    };


}
