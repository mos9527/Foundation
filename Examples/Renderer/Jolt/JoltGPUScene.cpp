#include "JoltGPUScene.hpp"

namespace Foundation::Examples
{
    namespace
    {
        PhysicsBodyPose ComposePose(PhysicsBodyPose const& body, PhysicsBodyPose const& local)
        {
            PhysicsBodyPose out;
            out.rotation = normalize(body.rotation * local.rotation);
            out.position = body.position + body.rotation * local.position;
            return out;
        }
    } // namespace

    JoltGPUScene::JoltGPUScene(RHIDevice* device, Core::JobSystem* jobs, JoltGPUSceneDesc const& desc,
                               Core::Allocator* allocator) :
        mWorld(jobs, desc.physics),
        mGPU(device, jobs, allocator, desc.gpu),
        mMaterials(allocator),
        mLights(allocator),
        mObjects(allocator),
        mFreeMaterials(allocator),
        mFreeLights(allocator)
    {
    }

    JoltGPUScene::~JoltGPUScene()
    {
        for (Object const& object : mObjects)
        {
            if (mWorld.IsBodyValid(object.body))
                (void)mWorld.DestroyBody(object.body);
        }
        mObjects.clear();
    }

    void JoltGPUScene::MarkBroadPhaseDirty() { mBroadPhaseDirty = true; }

    GPUScene::Result JoltGPUScene::Upload(FImportedMesh const& source, GeometryHandle& outHandle)
    {
        return mGPU.Upload(source, outHandle);
    }

    void JoltGPUScene::Join() { mGPU.Join(); }

    PhysicsStatus JoltGPUScene::CreateShape(PhysicsShapeDesc const& desc, PhysicsShapeHandle& outHandle)
    {
        return mWorld.CreateShape(desc, outHandle);
    }

    PhysicsStatus JoltGPUScene::DestroyShape(PhysicsShapeHandle handle)
    {
        return mWorld.DestroyShape(handle);
    }

    JoltGPUScene::MaterialSlot* JoltGPUScene::ResolveMaterial(GPUSceneMaterialHandle handle)
    {
        if (!handle.IsValid() || handle.index >= mMaterials.size())
            return nullptr;
        MaterialSlot& slot = mMaterials[handle.index];
        return slot.live && slot.generation == handle.generation ? &slot : nullptr;
    }

    JoltGPUScene::MaterialSlot const* JoltGPUScene::ResolveMaterial(GPUSceneMaterialHandle handle) const
    {
        return const_cast<JoltGPUScene*>(this)->ResolveMaterial(handle);
    }

    JoltGPUScene::LightSlot* JoltGPUScene::ResolveLight(GPUSceneLightHandle handle)
    {
        if (!handle.IsValid() || handle.index >= mLights.size())
            return nullptr;
        LightSlot& slot = mLights[handle.index];
        return slot.live && slot.generation == handle.generation ? &slot : nullptr;
    }

    PhysicsStatus JoltGPUScene::CreateMaterial(GSMaterial const& material, GPUSceneMaterialHandle& outHandle)
    {
        outHandle = {};
        uint32_t index;
        if (mFreeMaterials.empty())
        {
            index = static_cast<uint32_t>(mMaterials.size());
            mMaterials.push_back({});
        }
        else
        {
            index = mFreeMaterials.back();
            mFreeMaterials.pop_back();
        }

        MaterialSlot& slot = mMaterials[index];
        slot.live = true;
        slot.refCount = 0;
        slot.material = material;
        outHandle = {.index = index, .generation = slot.generation};
        return PhysicsStatus::Ok;
    }

    PhysicsStatus JoltGPUScene::SetMaterial(GPUSceneMaterialHandle handle, GSMaterial const& material)
    {
        MaterialSlot* slot = ResolveMaterial(handle);
        if (!slot)
            return PhysicsStatus::InvalidHandle;
        slot->material = material;
        return PhysicsStatus::Ok;
    }

    PhysicsStatus JoltGPUScene::DestroyMaterial(GPUSceneMaterialHandle handle)
    {
        MaterialSlot* slot = ResolveMaterial(handle);
        if (!slot)
            return PhysicsStatus::InvalidHandle;
        if (slot->refCount != 0)
            return PhysicsStatus::InvalidArgument;

        slot->live = false;
        slot->gpuIndex = ~0u;
        ++slot->generation;
        mFreeMaterials.push_back(handle.index);
        return PhysicsStatus::Ok;
    }

    PhysicsStatus JoltGPUScene::CreateLight(GSLight const& light, GPUSceneLightHandle& outHandle)
    {
        outHandle = {};
        bool const isEnvironment = (light.flags & kGSLightTypeMask) == kGSLightTypeEnvironment;
        if (isEnvironment)
        {
            for (LightSlot const& slot : mLights)
            {
                if (slot.live && (slot.light.flags & kGSLightTypeMask) == kGSLightTypeEnvironment)
                    return PhysicsStatus::InvalidArgument;
            }
        }

        uint32_t index;
        if (mFreeLights.empty())
        {
            index = static_cast<uint32_t>(mLights.size());
            mLights.push_back({});
        }
        else
        {
            index = mFreeLights.back();
            mFreeLights.pop_back();
        }

        LightSlot& slot = mLights[index];
        slot.live = true;
        slot.light = light;
        outHandle = {.index = index, .generation = slot.generation};
        return PhysicsStatus::Ok;
    }

    PhysicsStatus JoltGPUScene::SetLight(GPUSceneLightHandle handle, GSLight const& light)
    {
        LightSlot* slot = ResolveLight(handle);
        if (!slot)
            return PhysicsStatus::InvalidHandle;

        bool const isEnvironment = (light.flags & kGSLightTypeMask) == kGSLightTypeEnvironment;
        if (isEnvironment)
        {
            for (LightSlot const& other : mLights)
            {
                if (&other != slot && other.live &&
                    (other.light.flags & kGSLightTypeMask) == kGSLightTypeEnvironment)
                    return PhysicsStatus::InvalidArgument;
            }
        }
        slot->light = light;
        return PhysicsStatus::Ok;
    }

    PhysicsStatus JoltGPUScene::DestroyLight(GPUSceneLightHandle handle)
    {
        LightSlot* slot = ResolveLight(handle);
        if (!slot)
            return PhysicsStatus::InvalidHandle;
        slot->live = false;
        ++slot->generation;
        mFreeLights.push_back(handle.index);
        return PhysicsStatus::Ok;
    }

    PhysicsStatus JoltGPUScene::Spawn(PhysicsBodyDesc const& bodyDesc, PhysicsVisualDesc const& visualDesc,
                                      PhysicsBodyHandle& outBody)
    {
        outBody = {};
        MaterialSlot* material = ResolveMaterial(visualDesc.material);
        if (!material || !visualDesc.geometry.IsValid())
            return PhysicsStatus::InvalidHandle;

        PhysicsStatus const status = mWorld.CreateBody(bodyDesc, outBody);
        if (status != PhysicsStatus::Ok)
            return status;

        ++material->refCount;
        mObjects.push_back(Object{.body = outBody, .visual = visualDesc});
        MarkBroadPhaseDirty();
        return PhysicsStatus::Ok;
    }

    PhysicsStatus JoltGPUScene::Destroy(PhysicsBodyHandle body)
    {
        for (size_t i = 0; i < mObjects.size(); ++i)
        {
            Object const& object = mObjects[i];
            if (object.body != body)
                continue;

            PhysicsStatus const status = mWorld.DestroyBody(body);
            if (status != PhysicsStatus::Ok)
                return status;
            MaterialSlot* material = ResolveMaterial(object.visual.material);
            CHECK(material != nullptr && material->refCount > 0);
            --material->refCount;
            mObjects[i] = mObjects.back();
            mObjects.pop_back();
            MarkBroadPhaseDirty();
            return PhysicsStatus::Ok;
        }
        return PhysicsStatus::InvalidHandle;
    }

    bool JoltGPUScene::IsBodyValid(PhysicsBodyHandle handle) const { return mWorld.IsBodyValid(handle); }

    PhysicsStatus JoltGPUScene::SetObjectMaterial(PhysicsBodyHandle body, GPUSceneMaterialHandle materialHandle)
    {
        MaterialSlot* material = ResolveMaterial(materialHandle);
        if (!material)
            return PhysicsStatus::InvalidHandle;

        for (Object& object : mObjects)
        {
            if (object.body != body)
                continue;
            MaterialSlot* previous = ResolveMaterial(object.visual.material);
            CHECK(previous != nullptr && previous->refCount > 0);
            --previous->refCount;
            ++material->refCount;
            object.visual.material = materialHandle;
            return PhysicsStatus::Ok;
        }
        return PhysicsStatus::InvalidHandle;
    }

    PhysicsStatus JoltGPUScene::RayCast(PhysicsRayDesc const& ray, PhysicsRayHit& outHit) const
    {
        return mWorld.RayCast(ray, outHit);
    }

    PhysicsStatus JoltGPUScene::GetBodyPose(PhysicsBodyHandle body, PhysicsBodyPose& outPose) const
    {
        return mWorld.GetBodyPose(body, outPose);
    }

    PhysicsStatus JoltGPUScene::SetBodyPose(PhysicsBodyHandle body, PhysicsBodyPose const& pose,
                                            PhysicsActivation activation)
    {
        return mWorld.SetBodyPose(body, pose, activation);
    }

    PhysicsStatus JoltGPUScene::SetLinearVelocity(PhysicsBodyHandle body, float3 const& velocity)
    {
        return mWorld.SetLinearVelocity(body, velocity);
    }

    PhysicsStatus JoltGPUScene::Step(float deltaSeconds, int collisionSteps)
    {
        if (mBroadPhaseDirty)
        {
            mWorld.OptimizeBroadPhase();
            mBroadPhaseDirty = false;
        }
        return mWorld.Step(deltaSeconds, collisionSteps);
    }

    PhysicsStatus JoltGPUScene::Commit(RendererUBO& ubo)
    {
        uint32_t materialCount = 0;
        for (MaterialSlot& slot : mMaterials)
        {
            if (slot.live)
                slot.gpuIndex = materialCount++;
        }

        uint32_t lightCount = 0;
        LightSlot const* environment = nullptr;
        for (LightSlot const& slot : mLights)
        {
            if (!slot.live)
                continue;
            ++lightCount;
            if ((slot.light.flags & kGSLightTypeMask) == kGSLightTypeEnvironment)
                environment = &slot;
        }
        if (!environment)
            return PhysicsStatus::InvalidArgument;

        auto tables = mGPU.BeginScene(static_cast<uint32_t>(mObjects.size()), materialCount, lightCount);
        for (size_t i = 0; i < mObjects.size(); ++i)
        {
            Object const& object = mObjects[i];
            PhysicsBodyPose bodyPose{};
            PhysicsStatus const status = mWorld.GetBodyPose(object.body, bodyPose);
            if (status != PhysicsStatus::Ok)
                return status;

            MaterialSlot const* material = ResolveMaterial(object.visual.material);
            if (!material)
                return PhysicsStatus::InvalidHandle;

            PhysicsBodyPose const pose = ComposePose(bodyPose, object.visual.localPose);
            tables.instances[i] = GSInstance{
                .transform = pose.position,
                .rotation = pose.rotation,
                .scale = object.visual.scale,
                .materialIndex = material->gpuIndex,
                .resourceIndex = object.visual.geometry.index,
                .type = object.visual.type,
            };
        }

        for (MaterialSlot const& slot : mMaterials)
        {
            if (slot.live)
                tables.materials[slot.gpuIndex] = slot.material;
        }

        tables.lights[0] = environment->light;
        uint32_t lightIndex = 1;
        for (LightSlot const& slot : mLights)
        {
            if (slot.live && &slot != environment)
                tables.lights[lightIndex++] = slot.light;
        }

        mGPU.EndScene(tables);
        mGPU.UpdateUBO(ubo);
        return PhysicsStatus::Ok;
    }
} // namespace Foundation::Examples
