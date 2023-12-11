#ifndef SHARED_INCLUDE
#define SHARED_INCLUDE
#ifdef __cplusplus
#include "../../Common/Defines.hpp"
#pragma pack(push, 4) // otherwise it's 8 on 64-bit systems
#else
#ifndef SHARED_DEFINES
#define SHARED_DEFINES
#include "Defines.hpp"
#endif
// DirectX Types
typedef uint2 D3D12_GPU_VIRTUAL_ADDRESS;
struct D3D12_DRAW_INDEXED_ARGUMENTS
{
    uint IndexCountPerInstance;
    uint InstanceCount;
    uint StartIndexLocation;
    uint BaseVertexLocation;
    uint StartInstanceLocation;
};
struct D3D12_VERTEX_BUFFER_VIEW
{
    D3D12_GPU_VIRTUAL_ADDRESS BufferLocation;
    uint SizeInBytes;
    uint StrideInBytes;
};
struct D3D12_INDEX_BUFFER_VIEW
{
    D3D12_GPU_VIRTUAL_ADDRESS BufferLocation;
    uint SizeInBytes;
    uint Format;
};
#endif
struct IndirectCommand
{
    uint MeshIndex; // 1
    uint LodIndex; // 1
    D3D12_VERTEX_BUFFER_VIEW VertexBuffer; // 4
    D3D12_INDEX_BUFFER_VIEW IndexBuffer; // 4
    D3D12_DRAW_INDEXED_ARGUMENTS DrawIndexedArguments; // 5
};
struct IndirectCullCmdList { // ! align for CB
    uint cmdIndex;
    uint instanceAllowMask;
    uint instanceRejectMask;
    uint _pad;
};
struct InstanceCullConstant { // ! align for CB
    IndirectCullCmdList cmds[INSTANCE_CULL_MAX_CMDS];
    
    uint hizIndex;
    uint hizMips;
    uint2 pad_;
};
struct SceneCamera // ! align for CB
{
    float4 position; // 16

    float fov;
    float aspect;
    float nearZ;
    float farZ; // 16
	
    Plane clipPlanes[6];

    matrix view; 
    matrix projection;
    matrix viewProjection;

    matrix invView;
    matrix invProjection;
    matrix invViewProjection;

    float logLuminaceMin; // in EV scale
    float logLuminanceRange;
    float luminaceAdaptRate;
    float _pad;
};
#define IBL_PROBE_SPECULAR_GGX 0
#define IBL_PROBE_SPECULAR_SHEEN 1
struct SceneIBLProbe {
    uint cubemapHeapIndex;
    uint radianceHeapIndex;
    uint irradianceHeapIndex;
    uint lutHeapIndex;

    uint mips;
    uint enabled;    
    uint2 _pad;

    float diffuseIntensity;
    float specularIntensity;
    float occlusionStrength;
    float _pad2;
};
struct SkyboxConstants {
    uint enabled;
    uint radianceHeapIndex;
    float skyboxLod;
    float skyboxIntensity;    
};
struct SilhouetteConstants {
    float edgeThreshold;
    float3 edgeColor;

    uint frameBufferUAV;
    uint depthSRV;
    uint2 _pad;
};
struct ShadingConstants { // ! align for CB
    SceneIBLProbe iblProbe;

    uint ltcLutHeapIndex;
    uint3 _pad;

    uint tangentFrameSrv;
    uint gradientSrv;
    uint materialSrv;
    uint depthSrv;

    uint framebufferUav;
    uint3 _pad2;
};
struct TonemappingConstants { // ! align for CB
    uint hisotrgramUav;
    uint framebufferUav;
    uint avgLumUav;
    uint _pad;
};
struct SceneGlobals // ! align for CB
{
    SceneCamera camera;
    SceneCamera cameraPrev; // Two frame camera constant

    uint meshNumInstances;
    uint meshHeapIndex;

    uint lightNumInstances;
    uint lightHeapIndex;

    uint2 viewportDimension;
    uint2 frameDimension;

    uint sceneVersion;
    uint backBufferIndex;
    uint2 _pad2;

    float frameTime;
    float3 _pad3;
};
struct SceneMeshLod
{
    uint numIndices;
    uint numMeshlets;

    D3D12_INDEX_BUFFER_VIEW indices;
    D3D12_GPU_VIRTUAL_ADDRESS meshlets;
    D3D12_GPU_VIRTUAL_ADDRESS meshletTriangles;
    D3D12_GPU_VIRTUAL_ADDRESS meshletVertices;
};
struct SkinningConstants {
    uint numBones;
    uint numVertices;
    uint numShapeKeys;
    uint meshBufferIndex;
    D3D12_VERTEX_BUFFER_VIEW VB;
    SceneMeshLod IB[MAX_LOD_COUNT];
};
struct SceneMeshBuffer {
    BoundingBox boundingBox;

    D3D12_VERTEX_BUFFER_VIEW VB;
    SceneMeshLod IB[MAX_LOD_COUNT];
};
struct SceneMeshInstanceData
{
    uint version;
    uint instanceMeshLocalIndex; // VB/IB Buffer Index in Mesh buffers
    uint instanceMeshType;  // INSTANCE_MESH_TYPE_...
    uint instanceMaterialIndex; // Material index in Material buffers
    uint instanceFlags; 

    matrix transform;
    matrix transformInvTranspose;
    matrix transformPrev; // Transform from previous frame
};
struct SceneMaterial {
    // bindless handles within ResourceDescriptorHeap
    uint albedoMap; 
    uint normalMap;
    uint pbrMap;
    uint emissiveMap; // 4

    // plain old data
    float4 albedo;
    float4 pbr;
    float4 emissive;
};
#define SCENE_LIGHT_TYPE_POINT 0
#define SCENE_LIGHT_TYPE_SPOT 1
#define SCENE_LIGHT_TYPE_DIRECTIONAL 2
#define SCENE_LIGHT_TYPE_AREA_QUAD 3
#define SCENE_LIGHT_TYPE_AREA_LINE 4
#define SCENE_LIGHT_TYPE_AREA_DISK 5
struct SceneLight {
    uint enabled;
    uint type;    
    float4 position;
    float4 direction;
    float intensity;
    float4 color;
    /* Spot / Point Light */
    float spot_point_radius;
    /* Spot Light */
    float spot_size_rad;   // outer
    float spot_size_blend; // inner = size * blend
    /* Quad/Disk Area Light */
    float2 area_quad_disk_extents;
    uint area_quad_disk_twoSided;
    /* Line Area Light */
    float area_line_length;    
    float area_line_radius;
    uint  area_line_caps;
};
struct IndirectConstant
{
    uint MeshIndex;
};
struct FFXSpdConstant { // ! align for CB    
    uint4 dstMipHeapIndex[12]; // fill mip0 with src
    uint atomicCounterHeapIndex;
    uint numMips;
    uint numWorkGroups;
    uint pad_;
    uint2 dimensions;
};
struct DepthSampleToTextureConstant {
    uint depthSRVHeapIndex;
    uint hizMip0UavHeapIndex;
    uint2 dimensions;    
};
#ifdef __cplusplus
#pragma pack(pop)
#endif
#endif