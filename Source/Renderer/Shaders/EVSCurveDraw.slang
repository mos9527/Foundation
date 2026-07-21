// Vertex shader for DOTS curve triangle MDI draws. The static Primitive buffer is
// bound as a UINT32 index buffer; SV_VertexID is the curve-local vertex index.
// Leaf data reconstructs a radial normal / strand UV for the shared EPSGBuffer.
#include "ICommon.slang"

uniform UBO globalParams;
[[vk_binding(1, 0)]] StructuredBuffer<GSInstance> instances;
[[vk_binding(2, 0)]] ByteAddressBuffer primitive;
[[vk_binding(6, 0)]] StructuredBuffer<uint> drawInstanceIDs;

GSCurveLeaf LoadCurveLeaf(GSCurveSet curve, uint leafIndex)
{
    return primitive.Load<GSCurveLeaf>(curve.leaves.offset + leafIndex * sizeof(GSCurveLeaf));
}

[shader("vertex")]
V2F main(uint vertexID: SV_VertexID, uint drawID: SV_DrawIndex)
{
    uint instanceID = drawInstanceIDs[drawID];
    GSInstance inst = instances[instanceID];
    GSCurveSet curve = primitive.Load<GSCurveSet>(inst.resourceOffset);
    GSCurveDOTSVertex vtx =
        primitive.Load<GSCurveDOTSVertex>(curve.vertices.offset + vertexID * sizeof(GSCurveDOTSVertex));

    uint leafIndex = vertexID / 8u;
    GSCurveLeaf leaf = LoadCurveLeaf(curve, leafIndex);
    float3 axis = leaf.p1 - leaf.p0;
    float axisLen2 = dot(axis, axis);
    float3 tangent = axisLen2 > 1e-12f ? normalize(axis) : float3(0.0f, 1.0f, 0.0f);
    float localU = axisLen2 > 1e-12f ? saturate(dot(vtx.position - leaf.p0, axis) / axisLen2) : 0.0f;
    float3 axisPoint = lerp(leaf.p0, leaf.p1, localU);
    float3 radial = vtx.position - axisPoint;

    float3 normal;
    float3 bitangent;
    if (dot(radial, radial) > 1e-12f)
    {
        normal = normalize(radial);
        bitangent = cross(normal, tangent);
        if (dot(bitangent, bitangent) <= 1e-12f)
            CoordinateSystem(tangent, normal, bitangent);
        else
            bitangent = normalize(bitangent);
    }
    else
    {
        CoordinateSystem(tangent, normal, bitangent);
    }

    // DOTS leaf packs two orthogonal quads; odd corners of each quad are the -side edge.
    uint corner = vertexID % 8u;
    float h = (corner == 2u || corner == 3u || corner == 6u || corner == 7u) ? -1.0f : 1.0f;

    float3 position = inst.rotation.rotate(vtx.position) * inst.scale + inst.transform;
    float3 invScale = 1.0f / inst.scale;
    normal = normalize(inst.rotation.rotate(normal * invScale));
    tangent = normalize(inst.rotation.rotate(tangent * invScale));
    bitangent = normalize(inst.rotation.rotate(bitangent * invScale));

    V2F v2f;
    v2f.pos = mul(mul(globalParams.proj, globalParams.view), float4(position, 1.0));
    v2f.normal = normal;
    v2f.tangent = tangent;
    v2f.bitangent = bitangent;
    v2f.bitangentSign = 1.0f;
    v2f.uv = float2(lerp(leaf.u0, leaf.u1, localU), 0.5f * (h + 1.0f));
    v2f.meshlet = 0u;
    v2f.materialIndex = inst.materialIndex;
    v2f.instanceID = instanceID;
    return v2f;
}
