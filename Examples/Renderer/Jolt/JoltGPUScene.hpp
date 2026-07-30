#pragma once
#include "JoltPhysicsWorld.hpp"
#include <Renderer/GPUScene.hpp>
#include <Core/Container.hpp>

namespace Foundation::Examples
{
    struct GPUSceneMaterialHandle
    {
        uint32_t index{~0u};
        uint32_t generation{0};
        [[nodiscard]] bool IsValid() const { return index != ~0u; }
        [[nodiscard]] bool operator==(GPUSceneMaterialHandle const&) const = default;
    };

    struct GPUSceneLightHandle
    {
        uint32_t index{~0u};
        uint32_t generation{0};
        [[nodiscard]] bool IsValid() const { return index != ~0u; }
        [[nodiscard]] bool operator==(GPUSceneLightHandle const&) const = default;
    };

    struct PhysicsVisualDesc
    {
        GeometryHandle geometry{};
        GPUSceneMaterialHandle material{};
        uint32_t type{kGSInstanceTypeMesh};
        float3 scale{1.0f, 1.0f, 1.0f};
        PhysicsBodyPose localPose{};
    };

    struct JoltGPUSceneDesc
    {
        PhysicsWorldDesc physics{};
        GPUSceneDesc gpu{};
    };

    class JoltGPUScene
    {
    public:
        JoltGPUScene(RHIDevice* device, Core::JobSystem* jobs, JoltGPUSceneDesc const& desc = {},
                     Core::Allocator* allocator = GLOBAL_ALLOC);
        ~JoltGPUScene();

        JoltGPUScene(JoltGPUScene const&) = delete;
        JoltGPUScene& operator=(JoltGPUScene const&) = delete;

        [[nodiscard]] GPUScene& GetGPUScene() { return mGPU; }
        [[nodiscard]] GPUScene const& GetGPUScene() const { return mGPU; }

        [[nodiscard]] GPUScene::Result Upload(FImportedMesh const& source, GeometryHandle& outHandle);
        void Join();

        [[nodiscard]] PhysicsStatus CreateShape(PhysicsShapeDesc const& desc, PhysicsShapeHandle& outHandle);
        [[nodiscard]] PhysicsStatus DestroyShape(PhysicsShapeHandle handle);

        [[nodiscard]] PhysicsStatus CreateMaterial(GSMaterial const& material, GPUSceneMaterialHandle& outHandle);
        [[nodiscard]] PhysicsStatus SetMaterial(GPUSceneMaterialHandle handle, GSMaterial const& material);
        [[nodiscard]] PhysicsStatus DestroyMaterial(GPUSceneMaterialHandle handle);

        [[nodiscard]] PhysicsStatus CreateLight(GSLight const& light, GPUSceneLightHandle& outHandle);
        [[nodiscard]] PhysicsStatus SetLight(GPUSceneLightHandle handle, GSLight const& light);
        [[nodiscard]] PhysicsStatus DestroyLight(GPUSceneLightHandle handle);

        [[nodiscard]] PhysicsStatus Spawn(PhysicsBodyDesc const& bodyDesc, PhysicsVisualDesc const& visualDesc,
                                          PhysicsBodyHandle& outBody);
        [[nodiscard]] PhysicsStatus Destroy(PhysicsBodyHandle body);
        [[nodiscard]] bool IsBodyValid(PhysicsBodyHandle handle) const;
        [[nodiscard]] PhysicsStatus SetObjectMaterial(PhysicsBodyHandle body, GPUSceneMaterialHandle material);

        [[nodiscard]] PhysicsStatus RayCast(PhysicsRayDesc const& ray, PhysicsRayHit& outHit) const;
        [[nodiscard]] PhysicsStatus GetBodyPose(PhysicsBodyHandle body, PhysicsBodyPose& outPose) const;
        [[nodiscard]] PhysicsStatus SetBodyPose(PhysicsBodyHandle body, PhysicsBodyPose const& pose,
                                                PhysicsActivation activation = PhysicsActivation::Activate);
        [[nodiscard]] PhysicsStatus SetLinearVelocity(PhysicsBodyHandle body, float3 const& velocity);

        [[nodiscard]] PhysicsStatus Step(float deltaSeconds, int collisionSteps = 1);
        [[nodiscard]] PhysicsStatus Commit(RendererUBO& ubo);

        [[nodiscard]] uint32_t GetObjectCount() const { return static_cast<uint32_t>(mObjects.size()); }
        [[nodiscard]] uint32_t GetBodyCount() const { return mWorld.GetBodyCount(); }
        [[nodiscard]] uint32_t GetMaxBodies() const { return mWorld.GetMaxBodies(); }

    private:
        struct MaterialSlot
        {
            uint32_t generation{1};
            uint32_t refCount{0};
            uint32_t gpuIndex{~0u};
            bool live{false};
            GSMaterial material{};
        };

        struct LightSlot
        {
            uint32_t generation{1};
            bool live{false};
            GSLight light{};
        };

        struct Object
        {
            PhysicsBodyHandle body{};
            PhysicsVisualDesc visual{};
        };

        [[nodiscard]] MaterialSlot* ResolveMaterial(GPUSceneMaterialHandle handle);
        [[nodiscard]] MaterialSlot const* ResolveMaterial(GPUSceneMaterialHandle handle) const;
        [[nodiscard]] LightSlot* ResolveLight(GPUSceneLightHandle handle);
        void MarkBroadPhaseDirty();

        JoltPhysicsWorld mWorld;
        GPUScene mGPU;
        Core::Vector<MaterialSlot> mMaterials;
        Core::Vector<LightSlot> mLights;
        Core::Vector<Object> mObjects;
        Core::Vector<uint32_t> mFreeMaterials;
        Core::Vector<uint32_t> mFreeLights;
        bool mBroadPhaseDirty{false};
    };
} // namespace Foundation::Examples
