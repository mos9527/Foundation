#pragma once
#include "SceneData.hpp"
namespace Foundation {
    using namespace Foundation::RHI;
    using namespace Foundation::Core;
    class Scene {
    public:
        struct MeshMetadata {
            uint32_t vertexCount;
            uint32_t vertexStride;            
            uint32_t indexCount;
            uint32_t indexStride; // in bytes, usually 2 or 4
            /* raw vertex data of stride */
            /* raw index data of stride*/
            // TODO
            // meshlets?
            // lods?
        };
        struct InstanceMetadata {
            size_t primitiveOffset; // into m_data.GetPrimitiveDataBuffer()            
            /* raw opaque instance data*/
        };
        struct GlobalMetadata {
            /* raw opaque global data */
            size_t instanceCount;
            /* uint64_t offsets into m_data.GetInstanceDataBuffer() */
        };
    private:
        Allocator* m_allocator{ nullptr };
        Renderer& m_renderer;
        SceneData m_data;
        StlVector<uint64_t> m_instanceOffsets;

        bool m_hasDirtyInstance = false;
    public:
        Scene(Allocator* allocator, Renderer& renderer, SceneDataDesc const& desc);

        template<typename Vertex, typename Index>
        SceneHandle AddMesh(StlSpan<const Vertex> vertices, StlSpan<const Index> indices);
        void FreeMesh(SceneHandle handle);

        template<typename Instance>
        SceneHandle AddInstance(SceneHandle primitive, Instance const& data);
        template<typename Instance>
        void UpdateInstance(SceneHandle instance, SceneHandle primitive, Instance const& data);
        void FreeInstance(SceneHandle handle);

        template<typename Global>
        void UpdateGlobal(Global const& data);

        inline bool Update(RHICommandList* cmd) {
            return m_data.Update(cmd);
        }
    };
}
