import ICommon;
import IFlags;
import IMath;
import IGPUScene;
import IBindless;
import IScreenSpace;

public struct SceneRayHit
{
    public bool valid;
    public uint instance;
    public uint primitive;
    public float2 barycentrics;
    public float distance;
    public float3 position;
};

float2 SceneTriangleUV(ByteAddressBuffer staticPages, ByteAddressBuffer dynamicPages,
                       GSInstance instance, uint primitive, float2 bary)
{
    bool dynamic = GSInstanceIsDynamic(instance);
    GSMesh mesh = GSLoadPrimitive<GSMesh>(staticPages, dynamicPages, dynamic, instance.resourceOffset);
    float2 uv = float2(0.0f);
    float3 weights = float3(1.0f - bary.x - bary.y, bary);
    for (uint corner = 0u; corner < 3u; ++corner)
    {
        uint index = GSLoadPrimitive<uint>(staticPages, dynamicPages, dynamic,
            mesh.indices.offset + (primitive * 3u + corner) * 4u);
        FQVertex vertex = GSLoadPrimitive<FQVertex>(staticPages, dynamicPages, dynamic,
            mesh.vertices.offset + index * uint(sizeof(FQVertex)));
        uv += float2(vertex.uv) * weights[corner];
    }
    return uv;
}

uint SceneBayer2x2(uint2 pixel)
{
    return ((pixel.x ^ pixel.y) << 1u) | pixel.y;
}

// Match the raster coverage threshold for projected candidates. Outside the viewport,
// use a stable stochastic threshold; neither case evaluates secondary-hit lighting.
bool SceneCandidateCovered(UBO globals, ByteAddressBuffer instances, ByteAddressBuffer materials,
                           ByteAddressBuffer staticPages, ByteAddressBuffer dynamicPages,
                           SamplerState materialSampler, uint instanceIndex, uint primitive,
                           float2 bary, float3 position, uint2 sourcePixel)
{
    if (instanceIndex >= globals.instances.count)
        return false;
    GSInstance instance = GSLoad<GSInstance>(instances, globals.instances.offset + instanceIndex);
    // Curve BLAS triangles are geometry proxies, as in the existing raster RT shadow path.
    if (GSInstanceType(instance) == 1u)
        return true;
    GSMaterial material = GSLoad<GSMaterial>(materials, globals.materials.offset + instance.materialIndex);
    float opacity = material.baseColorFactor.a;
    if (material.baseColorTexture != ~0u)
    {
        float2 uv = SceneTriangleUV(staticPages, dynamicPages, instance, primitive, bary);
        opacity *= gTextures2D[NonUniformResourceIndex(material.baseColorTexture)]
            .SampleLevel(materialSampler, uv, 0.0f).a;
    }
    bool diffuseOnly = (globals.dbgMaterialFlags & uint32_t(MaterialFlagsBits.DbgDiffuseOnly)) != 0u;
    float transmission = diffuseOnly ? 0.0f : material.transmissionFactor;
    opacity = min(opacity, max(0.5f, 1.0f - transmission));
    if (opacity >= 1.0f)
        return true;
    float2 screenUV;
    float clipW;
    float threshold;
    if (ProjectScreenPosition(position, globals.view, globals.proj, screenUV, clipW))
    {
        uint2 pixel = uint2(screenUV * float2(globals.fbWidth, globals.fbHeight));
        threshold = (float(SceneBayer2x2(pixel & 1u) * 4u + SceneBayer2x2((pixel >> 1u) & 1u)) + 0.5f) / 16.0f;
    }
    else
        threshold = uintToFloat(hash(uint3(sourcePixel, primitive ^ instanceIndex)));
    return opacity >= threshold;
}

public SceneRayHit TraceSceneClosest(RaytracingAccelerationStructure scene, RayDesc ray, UBO globals,
                                    ByteAddressBuffer instances, ByteAddressBuffer materials,
                                    ByteAddressBuffer staticPages, ByteAddressBuffer dynamicPages,
                                    SamplerState materialSampler, uint2 sourcePixel)
{
    SceneRayHit hit;
    hit.valid = false;
    hit.instance = ~0u;
    hit.primitive = ~0u;
    hit.barycentrics = float2(0.0f);
    hit.distance = ray.TMax;
    hit.position = float3(0.0f);
    RayQuery<RAY_FLAG_NONE> query;
    // Geometry only: analytic-light AABBs (mask 0x02) must not become reflection surfaces.
    query.TraceRayInline(scene, RAY_FLAG_NONE, 0x01u | 0x04u, ray);
    while (query.Proceed())
    {
        if (query.CandidateType() == CANDIDATE_NON_OPAQUE_TRIANGLE &&
            SceneCandidateCovered(globals, instances, materials, staticPages, dynamicPages,
                                  materialSampler, query.CandidateInstanceID(), query.CandidatePrimitiveIndex(),
                                  query.CandidateTriangleBarycentrics(),
                                  ray.Origin + ray.Direction * query.CandidateTriangleRayT(), sourcePixel))
            query.CommitNonOpaqueTriangleHit();
    }
    if (query.CommittedStatus() == COMMITTED_TRIANGLE_HIT)
    {
        hit.valid = true;
        hit.instance = query.CommittedInstanceID();
        hit.primitive = query.CommittedPrimitiveIndex();
        hit.barycentrics = query.CommittedTriangleBarycentrics();
        hit.distance = query.CommittedRayT();
        hit.position = ray.Origin + ray.Direction * hit.distance;
    }
    return hit;
}
