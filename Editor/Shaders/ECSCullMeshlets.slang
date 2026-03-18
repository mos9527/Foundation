import ICommon;
import IFlags;
import IMath;
[[SpecializationConstant]] int cullFlags;
uniform UBO globalParams;

ByteAddressBuffer primitive;
StructuredBuffer<GSInstance> instances;

SamplerState hizSampler;
Texture2D<float> hiz;
RWStructuredBuffer<uint> visibility;

RWStructuredBuffer<uint> inTasksCounter;
RWStructuredBuffer<MeshletTaskWork> inTasks;

// vvv Cleared before dispatch
RWStructuredBuffer<Atomic<uint>> outMeshletCounter;
RWStructuredBuffer<uint2> outMeshletIndices; // (x: Meshlet ID, y: Instance ID)
// vvv Cleared before dispatch
RWStructuredBuffer<MeshletTaskDispatch> outMeshletDispatches;

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
// Returns (x1, y1, x2, y2) in UV space [0,1] from OpenGL NDC space
bool projSphereAABB(float3 c /* view */, float r /* scaled */, float P00, float P11, float zNear, out float4 aabb)
{
    // f = 1 / tan(fovY / 2), a = aspect ratio
    // P00 = f/a, P11 = f
	if (c.z + r > -zNear) // clipping near plane
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
	aabb = aabb.zyxw * float4(-0.5f, 0.5f, -0.5f, 0.5f) + float4(0.5f); // OGL clip space -> uv space

	return true;
}

// Each WG of task shader processes up to kMaxTaskShaderWork meshlets from the same Instance,
// and each thread in the WG processes one meshlet.
[shader("compute")]
[numthreads(kMeshWorkGroupSize, 1, 1)]
void main(uint groupIndex: SV_GroupIndex, uint2 groupID: SV_GroupID) {
    const bool early = cullFlags & kCullStageFirst;
    const bool late = cullFlags & kCullStageLate;
    const bool cullFrustum = cullFlags & kCullFrustum;
    const bool cullOcclusion = cullFlags & kCullOcclusion;
    // -- Global Load --
    uint count = inTasksCounter[0];
    uint dispatchIndex = dot(groupID, uint2(kMeshWorkGroupSize, 1)); // ID of this group, flattened    
    if (dispatchIndex >= count)
        return; // Group inactive. Should NOT happen.
    if (!cullOcclusion && late)
        return; // Misuse. If so Phase 2 should not be run at all.
    // -- Load --
    MeshletTaskWork work = inTasks[dispatchIndex];
    if (groupIndex >= work.numMeshlets)
        return; // Inactive
    GSInstance inst = instances[work.instanceID];
    GSMesh mesh = primitive.Load<GSMesh>(inst.meshOffset);
    uint32_t meshletID = work.firstMeshlet + groupIndex;
    float scale = max(max(inst.scale.x,inst.scale.y),inst.scale.z);
    // -- LOD selection --
    // Always runs - and deterministic regardless of cullFlags
    bool valid = false;
    float3 viewCenter;
    float radius;
    FMeshlet meshlet;
    {
        FLODGroup group;
        if (meshletID < mesh.meshletCount) {
            meshlet = primitive.Load<FMeshlet>(mesh.meshletOffset + meshletID * sizeof(FMeshlet));
            // View-dependent culling
            group = primitive.Load<FLODGroup>(mesh.groupOffset + meshlet.group * sizeof(FLODGroup));
            viewCenter = pointToView(group.center, inst, globalParams.view);
            float selfError = projErrorSimple(group.error, viewCenter, group.radius * scale, globalParams.proj[1][1], globalParams.zNear);
            // Remember - we take the upper_bound of the acceptable clusters
            // where it's the *first* LOD that exceeds the error threshold
            valid = selfError > globalParams.lodThreshold;
            if (meshlet.refined != ~0u){
                group = primitive.Load<FLODGroup>(mesh.groupOffset + meshlet.refined * sizeof(FLODGroup));
                viewCenter = pointToView(group.center, inst, globalParams.view);
                float refError = projErrorSimple(group.error, viewCenter, group.radius * scale, globalParams.proj[1][1], globalParams.zNear);
                // The refined ones exceeds it too - impossible unless it's closer to
                // the actual upper bound. So cull ourselves.
                valid &= refError <= globalParams.lodThreshold;
            }
        }
    }
    if (valid) {
        viewCenter = pointToView(meshlet.centerRadius.xyz, inst, globalParams.view);
        radius = meshlet.centerRadius.w * scale;
    }
    bool draw = valid;
    // -- Frustum Cull --
    if (cullFrustum && draw) {
        draw &= frustumCull(globalParams.projPlanes, viewCenter, radius, globalParams.zNear);
    }
    // -- Two-phase --
    uint32_t meshletGlobalID = mesh.meshletGlobalIndex + meshletID;
    if (cullOcclusion && draw) {
        // Draw during...
        // Early: visible in last frame?
        // Late:  *not* visible in Early?
        bool prev = (visibility[meshletGlobalID / 32] & (1u << (meshletGlobalID % 32))) != 0u;
        if (early && !prev) draw = false;
        if (late && prev) draw = false;
    }
    // -- Occlusion Cull --
    // Use HZB from early
    if (cullOcclusion && draw && late) {
        float4 aabb; // x1, y1, x2, y2 in UV
        const float hizWidth = globalParams.hizWidth;
        const float hizHeight = globalParams.hizHeight;
        if (projSphereAABB(viewCenter, radius, globalParams.proj[0][0], globalParams.proj[1][1], globalParams.zNear, aabb)) {
            // ^^^ Lets anything clipping the near plane pass
            float width = hizWidth * (aabb.z - aabb.x);
            float height = hizHeight * (aabb.w - aabb.y);
            float l = max(width, height);
            float lambda = ceil(log2(max(l, 1.0f)));
            float2 texel = (aabb.xy + aabb.zw) * 0.5f;
            float d = hiz.SampleLevel(hizSampler, texel, lambda).x;
            // View space depth *decreases* with distance
            float viewDepth = viewCenter.z + radius;
            float dMax = globalParams.zNear / -viewDepth;
            bool pass = d <= dMax;
            draw &= pass;
         }
    }
    // Commit to visibility buffer for the next frame
    if (cullOcclusion && valid && late) {
        if (draw)
            InterlockedOr(visibility[meshletGlobalID / 32], 1u << (meshletGlobalID % 32));
        else
            InterlockedAnd(visibility[meshletGlobalID / 32], ~(1u << (meshletGlobalID % 32)));
    }
    // Submit
    uint waveDrawCount = WaveActiveCountBits(draw);
    if (waveDrawCount > 0){
        uint waveMeshletOffset = 0;
        if (WaveIsFirstLane())
            waveMeshletOffset = outMeshletCounter[0].add(waveDrawCount);
        // Boardcast
        waveMeshletOffset = WaveReadLaneFirst(waveMeshletOffset);
        // Spill the big indices list into multiple dispatches
        // of kMaxMeshletsPerDispatch each.
        uint waveMeshletCount = waveMeshletOffset + waveDrawCount;
        waveMeshletCount = WaveReadLaneFirst(waveMeshletCount);
        if (WaveIsFirstLane()){
            uint groupCountX = (waveMeshletCount + kMeshIndirectPerDrawMeshlets - 1) / kMeshIndirectPerDrawMeshlets;
            uint groupCountY = min(waveMeshletCount, kMeshIndirectPerDrawMeshlets);
            uint groupCountZ = 1;
            InterlockedMax(outMeshletDispatches[0].groupCountX, groupCountX);
            InterlockedMax(outMeshletDispatches[0].groupCountY, groupCountY);
            outMeshletDispatches[0].groupCountZ = 1;
        }
        if (draw) {
            // Output the visible meshlet
            uint pos = WavePrefixCountBits(draw);
            uint outPos = waveMeshletOffset + pos;
            outMeshletIndices[outPos] = uint2(meshletID, work.instanceID);
        }
    }
    // If this was a Task Shader (which it was): DispatchMesh(shared.meshletCount,1,1,shared);
    // But - Don't bother. See also https://themaister.net/blog/2024/01/17/modernizing-granites-mesh-rendering/
    // on "Task Shader Woes".
    // Also, I couldn't get this to work with AMDPRO drivers anyway. We'll aggregate
    // the mesh dispatches here and use vkCmdDrawMeshTasksIndirectCountEXT without involving Task Shader later.
    //
    // An added benefit is that we can also use this buffer to use traditional raster pipeline if needed
    // since Vulkan supports 8-bit indices for mesh tasks indirect draws.
    // Alan Wake 2 falls back to this path (and is thus indescribably slow) on older GPUs.
}
