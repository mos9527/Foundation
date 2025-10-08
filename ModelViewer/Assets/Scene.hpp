#pragma once
#include <Core/Pool.hpp>
#include <Rendering/GPUScene.hpp>
#include "Mesh.hpp"
namespace ModelViewer {
    using namespace Foundation;
    using namespace Rendering;
    using SceneHandle = uint32_t;
    constexpr uint32_t kSceneInvalid = ~0u;
    constexpr int kMaxSceneMeshLodCount = 4;
    /* -- Mesh -- */
    // Intermediate scratch buffers
    // TODO: We may want to actually cache these. Meshlet generation can be expensive.
    struct SceneMeshLodData
    {
        Vector<MeshIndex> indices;
        Vector<MeshMeshlet> meshlets;
        Vector<MeshIndex> meshletVertices;
        Vector<MeshMicroIndex> meshletTriangles;
        SceneMeshLodData(Allocator* allocator) : indices(allocator), meshlets(allocator), meshletVertices(allocator), meshletTriangles(allocator) {}
    };
    struct SceneMeshData
    {
        Vector<MeshVertexCompact> vertices;
        Vector<SceneMeshLodData> lods;
        SceneMeshData(Allocator* allocator) : vertices(allocator), lods(allocator) {}
    };
    SceneMeshData sceneMeshDataFromVertexIndex(Span<MeshVertex> vertices, Span<MeshIndex> indices, Allocator* allocator, int numLods = kMaxSceneMeshLodCount, bool buildMeshlets = true);
#pragma pack(push, 4)
    struct SceneMeshLodAllocation
    {
        VirtualAllocation indices{kInvalidVirtualAllocation};
        VirtualAllocation meshlets{kInvalidVirtualAllocation};
        VirtualAllocation meshletVertices{kInvalidVirtualAllocation};
        VirtualAllocation meshletTriangles{kInvalidVirtualAllocation};
        uint32_t indexCount;
        uint32_t meshletCount;

        uint32_t indexRawOffset;
        uint32_t meshletRawOffset;
        uint32_t meshletVerticesRawOffset;
        uint32_t meshletTrianglesRawOffset;
    };
    /**
     * 4-byte aligned. Allocated in the Const buffer in the @ref GPUScene
     * @ref VirtualAllocation fields are unused in the shaders.
     */
    struct SceneMeshAllocation
    {
        VirtualAllocation vertices{kInvalidVirtualAllocation};        
        uint32_t vertexCount;
        uint32_t vertexRawOffset;
        uint32_t lodCount;
        SceneMeshLodAllocation lods[kMaxSceneMeshLodCount];
        // We store ourselves in the Const buffer as well
        VirtualAllocation self{kInvalidVirtualAllocation};
        uint32_t selfRawOffset;
    };
    /* -- Instance -- */
    /**
     * 4-byte aligned. Allocated in the Instance buffer in the @ref GPUScene
     */
    struct SceneInstanceData
    {
        float3 t; // Translation
        quat q; // Rotation Quat (xyzw)
        float3 s; // Scale
        // @ref SceneMeshAllocation::selfRawOffset + 1
        // 0 reserved for no mesh
        uint32_t meshAllocationRawOffsetPP = 0;            
    };
    /* -- Metadata -- */
    struct SceneCamera
    {
        float3 position{1,1,1};
        float3 lookAt{0,0,0};
        float3 up{0, 0, 1};

        float verticalFov{radians(45.0)}; // In radians
        float aspectRatio{1};
        float zNear{1e-3};
        
        struct Params
        {
            mat4 viewProj;
            float3 cameraPosition;
            float zNear;
        };
        Params GetParams() const;
    };
    struct SceneGrid
    {
        uint32_t dimension{10000};
        float width{0.01};
        enum class Type : uint32_t
        {
            Cartesian = 0,
            Radial = 1
        } type{Type::Cartesian};

        struct Params
        {
            SceneCamera::Params camera;
            uint dimension;
            float width;
            uint type;
        };
        Params GetParams(SceneCamera const& camera) const;
    };
    class Scene
    {
        Allocator* mAllocator;
        GPUScene* mGPUScene;

        Pool<SceneHandle, SceneMeshAllocation> mMeshes;
    public:
        Scene(GPUScene* scene, Allocator* allocator);

        float mTime{};
        
        /* -- Data -- */
        // TODO: Ugly - we do have a upper bound and data are in fact expected
        //       to be tightly packed. This works for now.
        uint32_t mInstanceCount{0};
        SceneCamera mCamera{};
        SceneGrid mGrid{};

        struct Params
        {
            SceneCamera::Params camera;
            uint32_t instanceCount;
            uvec3 _padding;
        };
        Params GetParams() const;

        SceneHandle PushMesh(SceneMeshData const& data);
        SceneMeshAllocation const& QueryMesh(SceneHandle handle);
        void FreeMesh(SceneHandle handle);
        
        Span<SceneInstanceData> MapInstances();
        void UnmapInstances();
    };
#pragma pack(pop)
}