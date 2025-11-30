#include "EMeshletCommon.slangi"
[[SpecializationConstant]] int cullFlags;

uniform UBO globalParams;
StructuredBuffer<GSInstance> instances;
ByteAddressBuffer primitive;
RWStructuredBuffer<MeshletTaskWork> tasks;
RWStructuredBuffer<Atomic<uint>> counter;
[[vk_binding(5,0)]] SamplerState hizSampler;
[[vk_binding(6,0)]] Texture2D<float> hiz;
[[vk_binding(7,0)]] RWStructuredBuffer<Atomic<uint>> occlusion;

// Model to view space
float3 pointToView(float3 point, GSInstance inst, float4x4 view) {
    float3 worldPos = inst.rotation.rotate(point) * inst.scale + inst.transform;
    return mul(view, float4(worldPos, 1.0)).xyz;
}

bool frustumCull(float4 ijkl /* [left ac:ij] [top bc:kl] */, float3 center /* view */, float radius /* scaled */, float zNear) {

    float ix = abs(ijkl.x * center.x);
    float ky = abs(ijkl.z * center.y);
    float jz = ijkl.y * center.z;
    float lz = ijkl.w * center.z;
    return (center.z - radius <= zNear) && (jz >= ix - radius) && (lz >= ky - radius);
}

float projErrorSimple(float error, float3 center /* view */, float radius /* scaled */, float P11, float zNear) {
    float d = max(length(center) - radius, zNear);
    return error / d * abs(P11) * 0.5f;
}
// 2D Polyhedral Bounds of a Clipped, Perspective-Projected 3D Sphere. Michael Mara, Morgan McGuire. 2013
// Returns (x1, y1, x2, y2) in UV space [0,1], Y goes down
bool projSphereAABB(float3 c /* view */, float r /* scaled */, float P00, float P11, float zNear, out float4 aabb)
{
    // f = 1 / tan(fovY / 2), a = aspect ratio
    // P00 = f/a, P11 = f
	if (c.z + r > zNear) // clipping near plane
		return false;

	float3 cr = c * r;
	float czr2 = c.z * c.z - r * r;

	float vx = sqrt(c.x * c.x + czr2);
	float minx = (vx * c.x - cr.z) / (vx * c.z + cr.x);
	float maxx = (vx * c.x + cr.z) / (vx * c.z - cr.x);

	float vy = sqrt(c.y * c.y + czr2);
	float miny = (vy * c.y - cr.z) / (vy * c.z + cr.y);
	float maxy = (vy * c.y + cr.z) / (vy * c.z - cr.y);

	aabb = float4(minx * P00, miny * P11, maxx * P00, maxy * P11);
	aabb = aabb.xwzy * float4(0.5f, -0.5f, 0.5f, -0.5f) + float4(0.5f); // clip space -> uv space

	return true;
}

groupshared MeshletGroupShared shared;

// Each WG of task shader processes up to kMaxTaskShaderWork meshlets from the same Instance,
// and each thread in the WG processes one meshlet.
[shader("amplification")]
[numthreads(kTaskShaderWG, 1, 1)]
void main(uint groupIndex: SV_GroupIndex, uint2 groupID: SV_GroupID) {
    const bool early = cullFlags & kCullStageFirst;
    const bool late = cullFlags & kCullStageLate;
    const bool cullFrustum = cullFlags & kCullFrustum;
    const bool cullOcclusion = cullFlags & kCullOcclusion;

    uint count = counter[0].load();
    uint dispatchIndex = dot(groupID, uint2(kTaskShaderWG, 1)); // ID of this group, flattened    
    if (dispatchIndex >= count)
        return; // Group inactive. Should NOT happen.
    if (!cullOcclusion && late)
        return; // Misuse. If so Phase 2 should not be run at all.
    // -- Load --
    MeshletTaskWork work = tasks[dispatchIndex];
    GSInstance inst = instances[work.instanceID];
    GSMesh mesh = primitive.Load<GSMesh>(inst.meshOffset);
    uint32_t meshletID = work.firstMeshlet + groupIndex;
    // -- Init --
    shared.instanceID = work.instanceID;
    shared.meshletCount = 0;
    GroupMemoryBarrierWithGroupSync();
    float scale = max(max(inst.scale.x,inst.scale.y),inst.scale.z);
    // -- LOD selection --
    // Always runs - and deterministic regardless of cullFlags
    bool visible = false;
    float3 viewCenter;
    float radius;
    FMeshlet meshlet = primitive.Load<FMeshlet>(mesh.meshletOffset + meshletID * sizeof(FMeshlet));
    {
        FLODGroup group;
        if (meshletID < mesh.meshletCount) {
            // View-dependent culling
            group = primitive.Load<FLODGroup>(mesh.groupOffset + meshlet.group * sizeof(FLODGroup));
            viewCenter = pointToView(group.center, inst, globalParams.view);
            float selfError = projErrorSimple(group.error, viewCenter, group.radius * scale, globalParams.proj[1][1], globalParams.zNear);
            // Remember - we take the upper_bound of the acceptable clusters
            // where it's the *first* LOD that exceeds the error threshold
            visible = selfError > globalParams.lodThreshold;
            if (meshlet.refined != ~0u){
                group = primitive.Load<FLODGroup>(mesh.groupOffset + meshlet.refined * sizeof(FLODGroup));
                viewCenter = pointToView(group.center, inst, globalParams.view);
                float refError = projErrorSimple(group.error, viewCenter, group.radius * scale, globalParams.proj[1][1], globalParams.zNear);
                // The refined ones exceeds it too - impossible unless it's closer to
                // the actual upper bound. So cull ourselves.
                visible &= refError <= globalParams.lodThreshold;
            }
        }
    }
    if (visible){
        viewCenter = pointToView(meshlet.centerRadius.xyz, inst, globalParams.view);
        radius = meshlet.centerRadius.w * scale;
    }
    // -- Cone Cull (FIXME. False positives.) --
    if (false && visible) {
        float3 coneApex = inst.rotation.rotate(meshlet.coneApex) * inst.scale + inst.transform;
        float3 camPosition = -float3(globalParams.view[0][3],globalParams.view[1][3],globalParams.view[2][3]);
        bool pass = dot(normalize(coneApex - camPosition), meshlet.coneAxisAngle.xyz) < meshlet.coneAxisAngle.w;
        visible &= pass;
    }
    // -- Frustum Cull --
    if (cullFrustum && visible){
        visible &= frustumCull(globalParams.projPlanes, viewCenter, radius, globalParams.zNear);
    }
    // -- Occlusion Cull --
    uint32_t meshletGlobalID = mesh.meshletGlobalIndex + meshletID;
    if (cullOcclusion && visible) {
        if (late) {
            uint32_t phase1Visible = occlusion[meshletGlobalID / 32].load() & (1u << (meshletGlobalID % 32));
            if (phase1Visible != 0)
                visible = false; // Skip
        }
        if (visible) {
            float4 aabb; // x1, y1, x2, y2 in UV
            // Let anything past the near plane pass
            const float hizWidth = globalParams.hizWidth;
            const float hizHeight = globalParams.hizHeight;
            if (projSphereAABB(viewCenter, radius, globalParams.proj[0][0], globalParams.proj[1][1], globalParams.zNear, aabb)) {
                float width = hizWidth * (aabb.z - aabb.x);
                float height = hizHeight * (aabb.w - aabb.y);
                float l = max(width,height);
                float lambda = min(ceil(log2(max(l, 1.0f))), globalParams.hizLevels - 1.0f);
                float2 texel = (aabb.xy + aabb.zw) / 2.0f;
                float d = hiz.SampleLevel(hizSampler, texel, lambda).x;
                // View space depth *decreases* with distance
                float viewDepth = viewCenter.z + radius;
                float dMax = globalParams.zNear / -viewDepth;
                bool pass = d <= dMax;
                if (!pass)
                    printf("reject %d l=%.3f depth=%.3f d=%.3f uv=(%.3f,%.3f)\n", meshletGlobalID, lambda, dMax, d, texel.x, texel.y);
                visible &= pass;
                // Commit to occlusion buffer if visible ONLY in early pass
                if (early) {
                    if (visible)
                        occlusion[meshletGlobalID / 32].or(1u << (meshletGlobalID % 32));
                }
            }
        }
    }
    if (visible) {
        // Output the visible meshlet
        // TODO: Wave intrinsics. See // https://gpuopen.com/learn/mesh_shaders/mesh_shaders-optimization_and_best_practices/
        uint sharedCount;
        InterlockedAdd(shared.meshletCount,1,sharedCount);
        shared.meshletIndices[sharedCount] = meshletID;
    }
    GroupMemoryBarrierWithGroupSync();
    DispatchMesh(shared.meshletCount,1,1,shared);
}