// TODO: Codegen for these structs
module Definitions;
import "../Math/Quaternion.slang";
import "../Math/Bits.slang";
public static const uint32_t kMaxSceneMeshLodCount = 4;
public static const uint32_t kMaxTaskShaderWork = 64; 
public static const uint32_t kMaxMeshShaderWork = 64;
// Work group size per task shader - 64 usually fits wave sizes on most HW
public struct SceneMeshLodAllocation
{
    uint32_t indices;
    uint32_t meshlets;
    uint32_t meshletVertices;
    uint32_t meshletTriangles;

    public uint32_t indexCount;
    public uint32_t meshletCount;
    public uint32_t indexRawOffset;
    public uint32_t meshletRawOffset;
    public uint32_t meshletVerticesRawOffset;
    public uint32_t meshletTrianglesRawOffset;
};
public struct SceneMeshAllocation
{
    uint32_t vertices;    
    uint32_t vertexCount;

    public uint32_t vertexRawOffset;
    public uint32_t lodCount;
    public SceneMeshLodAllocation lods[kMaxSceneMeshLodCount];

    uint32_t self;
    uint32_t selfRawOffset;
};
public struct SceneInstanceData
{
    public float3 t;
    public Quaternion q;
    public float3 s;
    public uint32_t meshAllocationRawOffsetPP;    
};
public struct MeshletTaskParams
{
    public uint32_t instanceID;
    public uint32_t vertexRawOffset;
    public uint32_t meshletCount;
    public uint32_t meshletRawOffset;
    public uint32_t meshletVerticesRawOffset;
    public uint32_t meshletTrianglesRawOffset;
};
public struct MeshMeshlet
{
    public uint32_t vertexOffset;
    public uint32_t triangleOffset;
    public uint32_t vertexCount;
    public uint32_t triangleCount;
    public float4 sphereCenterRadius; // (x,y,z,r)
    public float4 coneAxisAngle; // (x,y,z,cos(half solid angle))
    public float3 coneApex;
};
public typedef uint8_t MeshMicroIndex;
public struct MeshletTaskDispatch // VkDrawMeshTasksIndirectCommandEXT
{
    public uint32_t groupCountX;
    public uint32_t groupCountY;
    public uint32_t groupCountZ;
};
public struct MeshletPayload
{
    public uint32_t meshletIndices[kMaxTaskShaderWork];
    public uint32_t meshletRawOffset;
    public uint32_t vertexRawOffset;
    public uint32_t meshletCount;
    public uint32_t instanceID;
    public uint32_t meshletVerticesRawOffset;
    public uint32_t meshletTrianglesRawOffset;
};
public struct MeshVertexCompact {
    public half px, py, pz; // quantized fp16
    public uint16_t tp;     // tangent [octa 8+8]
    public uint32_t np;     // normal packed [snorm octa 15+15, bitangent sign 2]
    public half u, v;       // texcoord fp16
};
public struct VertexOutput
{    
    public float4 pos : SV_Position;
    public float3 normal;
    public float3 tangent;
    public float3 bitangent;
    public float2 uv;
    public uint meshlet;
};
public struct PrimitiveOutput
{
    public uint id : SV_PrimitiveID;
    public bool cull : SV_CullPrimitive;
}
public struct CameraParams {
    public float4x4 viewProj;    
    public float4 cameraPosition; // xyz = position, w = zNear    
}
public struct CullParams {
    public float4x4 viewMatrix;
    public float4x4 viewProj;
    public float4 frustumACBC; // left(ax,0,cz,0) top(0,by,cz,0) -> acbc coefficients
    public float3 cameraPosition;
    public float zCull; // Max z for coarse culling
}
public struct SceneParams {
    public CameraParams camera;
    public CullParams culling;
    public uint32_t instanceCount;
}
