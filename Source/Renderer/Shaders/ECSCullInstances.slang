import ICommon;
import IFlags;
import IHiZ;
import IMath;

[[SpecializationConstant]] int cullFlags;
uniform UBO globalParams;

RWStructuredBuffer<Atomic<uint>> outTasksCounter;
RWStructuredBuffer<MeshletTaskWork> outTasks;
StructuredBuffer<GSInstance> instances;
ByteAddressBuffer primitive;
RWStructuredBuffer<MeshletTaskDispatch> outTasksDispatch;

RWStructuredBuffer<Atomic<uint>> occludedInstancesCounter;
RWStructuredBuffer<uint> occludedInstances;
RWStructuredBuffer<MeshletTaskDispatch> occludedInstancesDispatch;
RWStructuredBuffer<uint> visibility;

SamplerState hizSampler;
Texture2D<float> hiz;

bool frustumCull(float4 ijkl, float3 center, float radius, float zNear)
{
    float ix = abs(ijkl.x * center.x);
    float ky = abs(ijkl.z * center.y);
    float jz = ijkl.y * center.z;
    float lz = ijkl.w * center.z;
    return (center.z - radius <= zNear) && (jz >= ix - radius) && (lz >= ky - radius);
}

float3 pointToWorld(float3 point, GSInstance inst)
{
    return inst.rotation.rotate(point) * inst.scale + inst.transform;
}

bool projectAABB(GSMesh mesh, GSInstance inst, float4x4 viewProj, out float4 aabb, out float nearestDepth)
{
    float2 rectMin = float2(1.0f);
    float2 rectMax = float2(0.0f);
    nearestDepth = 0.0f;
    for (uint corner = 0; corner < 8; corner++) {
        float3 sign = float3((corner & 1) ? 1.0f : -1.0f,
                             (corner & 2) ? 1.0f : -1.0f,
                             (corner & 4) ? 1.0f : -1.0f);
        float3 world = pointToWorld(mesh.boundsCenter + sign * mesh.boundsExtent, inst);
        float4 clip = mul(viewProj, float4(world, 1.0f));
        if (clip.w <= 0.0f || clip.z > clip.w)
            return false;
        float3 ndc = clip.xyz / clip.w;
        float2 uv = ndc.xy * float2(0.5f, -0.5f) + 0.5f;
        rectMin = min(rectMin, uv);
        rectMax = max(rectMax, uv);
        nearestDepth = max(nearestDepth, ndc.z);
    }
    aabb = float4(rectMin, rectMax);
    return true;
}

void emitTasks(uint iid, GSMesh mesh, bool clearVisibility)
{
    uint groups = alignUp(mesh.meshlets.count, kMeshWorkGroupSize) / kMeshWorkGroupSize;
    uint taskIndex = outTasksCounter[0].add(groups);
    for (uint group = 0; group < groups; group++) {
        MeshletTaskWork task;
        task.instanceID = iid;
        task.numMeshlets = min(mesh.meshlets.count - group * kMeshWorkGroupSize, kMeshWorkGroupSize);
        task.firstMeshlet = group * kMeshWorkGroupSize;
        outTasks[taskIndex + group] = task;
        if (clearVisibility) {
            visibility[(taskIndex + group) * 2] = 0u;
            visibility[(taskIndex + group) * 2 + 1] = 0u;
        }
    }
    uint groupIndex = WaveActiveMax(taskIndex + groups);
    if (WaveIsFirstLane()) {
        uint groupX = (groupIndex + kMeshWorkGroupSize - 1) / kMeshWorkGroupSize;
        uint groupY = min(groupIndex, kMeshWorkGroupSize);
        InterlockedMax(outTasksDispatch[0].groupCountX, groupX);
        InterlockedMax(outTasksDispatch[0].groupCountY, groupY);
        outTasksDispatch[0].groupCountZ = 1;
    }
}

bool loadInstanceMesh(uint iid, out GSInstance inst, out GSMesh mesh)
{
    inst = instances[iid];
    if (GSInstanceType(inst) != 0u || GSInstanceIsDynamic(inst))
        return false;
    mesh = primitive.Load<GSMesh>(inst.resourceOffset);

    float3 worldCenter = pointToWorld(mesh.boundsCenter, inst);
    float3 viewCenter = mul(globalParams.view, float4(worldCenter, 1.0f)).xyz;
    float radius = length(mesh.boundsExtent) * max(max(abs(inst.scale.x), abs(inst.scale.y)), abs(inst.scale.z));
    if ((cullFlags & uint32_t(CullFlagsBits.Frustum)) != 0 &&
        !frustumCull(globalParams.projPlanes, viewCenter, radius, globalParams.zNear))
        return false;
    return true;
}

bool isOccluded(GSMesh mesh, GSInstance inst, float4x4 viewProj)
{
    float4 aabb;
    float nearestDepth;
    if (!projectAABB(mesh, inst, viewProj, aabb, nearestDepth))
        return false;
    float hizDepth = SampleHiZConservative(hiz, hizSampler, aabb,
                                           uint2(globalParams.hizWidth, globalParams.hizHeight),
                                           globalParams.hizLevels);
    return nearestDepth < hizDepth;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void main(uint tid: SV_DispatchThreadID)
{
    if (tid >= globalParams.instances.count)
        return;
    uint iid = tid + globalParams.instances.offset;
    GSInstance inst;
    GSMesh mesh;
    if (!loadInstanceMesh(iid, inst, mesh))
        return;

    if ((cullFlags & uint32_t(CullFlagsBits.Occlusion)) != 0 && globalParams.frameNumber > 0 &&
        isOccluded(mesh, inst, globalParams.previousViewProj)) {
        uint offset = occludedInstancesCounter[0].add(1);
        occludedInstances[offset] = iid;
        uint end = WaveActiveMax(offset + 1);
        if (WaveIsFirstLane()) {
            InterlockedMax(occludedInstancesDispatch[0].groupCountX,
                           (end + kMeshWorkGroupSize - 1) / kMeshWorkGroupSize);
            occludedInstancesDispatch[0].groupCountY = 1;
            occludedInstancesDispatch[0].groupCountZ = 1;
        }
        return;
    }

    emitTasks(iid, mesh, false);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void post(uint tid: SV_DispatchThreadID)
{
    if (tid >= occludedInstancesCounter[0].load())
        return;
    uint iid = occludedInstances[tid];
    GSInstance inst;
    GSMesh mesh;
    if (!loadInstanceMesh(iid, inst, mesh))
        return;

    float4x4 viewProj = mul(globalParams.proj, globalParams.view);
    if ((cullFlags & uint32_t(CullFlagsBits.Occlusion)) != 0 && isOccluded(mesh, inst, viewProj))
        return;
    emitTasks(iid, mesh, true);
}