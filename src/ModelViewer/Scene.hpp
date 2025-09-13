#pragma once
#include <Rendering/StagingBuffer.hpp>
#include "Mesh.hpp"
namespace Foundation {
    using namespace Foundation::RHI;
    using namespace Foundation::Core;
    using namespace Foundation::Rendering;
    using namespace Foundation::Math;
    #include "Shaders/Common.h"
    using MeshHandle =  Tuple<DataHandle, DataHandle, DataHandle>;
    inline DataHandle PrimitiveIDOf(MeshHandle const& handle) { return std::get<0>(handle); }
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
        DataHandle AddMeshSpan(Span<const Vertex> vertices, Span<const Index> indices, DataHandle& outVtx, DataHandle& outIdx);
    public:
        Scene(Allocator* allocator, RHIDevice* device, SceneDataDesc const& desc);
        MeshHandle AddMesh(Mesh const& mesh) {
            DataHandle outVtx, outIdx;
            DataHandle handle = AddMeshSpan(
                Span<const Vertex>{reinterpret_cast<const Vertex*>(mesh.m_vertex_data.data()), mesh.m_num_vertices},
                Span<const Index>{reinterpret_cast<const Index*>(mesh.m_index_data.data()), mesh.m_num_indices},
                outVtx, outIdx
            );
            return {handle, outVtx, outIdx};
        }
        void FreeMesh(MeshHandle handle);

        DataHandle AddInstance(InstanceMetadata data);
        void UpdateInstance(DataHandle instance, InstanceMetadata const& data);
        void FreeInstance(DataHandle handle);

        auto* CreateUpdatePass(
            Renderer* renderer,
            RHIDeviceQueueType queue,
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
                queue,
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
                    return !HasUpdates();
                }
            );
        }
    };


}
