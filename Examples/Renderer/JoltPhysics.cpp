#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>

#include <algorithm>
#include <iostream>
#include <vector>
#include <thread>
#include "Examples.hpp"
#include "Jolt/JoltCommon.hpp"

#include <Renderer/Mesh.hpp>
#include <Renderer/Rasterizer.hpp>
#include <Renderer/Pathtracer.hpp>

JPH_SUPPRESS_WARNINGS
using namespace JPH;
using namespace JPH::literals;
using namespace std;

namespace Layers
{
    static constexpr ObjectLayer NON_MOVING = 0;
    static constexpr ObjectLayer MOVING = 1;
    static constexpr ObjectLayer NUM_LAYERS = 2;
}

class ObjectLayerPairFilterImpl : public ObjectLayerPairFilter
{
public:
    virtual bool ShouldCollide(ObjectLayer inObject1, ObjectLayer inObject2) const override
    {
        switch (inObject1)
        {
        case Layers::NON_MOVING:
            return inObject2 == Layers::MOVING; 
        case Layers::MOVING:
            return true; 
        default:
            return false;
        }
    }
};

namespace BroadPhaseLayers
{
    static constexpr BroadPhaseLayer NON_MOVING(0);
    static constexpr BroadPhaseLayer MOVING(1);
    static constexpr uint NUM_LAYERS(2);
}

class BPLayerInterfaceImpl final : public BroadPhaseLayerInterface
{
public:
    BPLayerInterfaceImpl()
    {
        mObjectToBroadPhase[Layers::NON_MOVING] = BroadPhaseLayers::NON_MOVING;
        mObjectToBroadPhase[Layers::MOVING] = BroadPhaseLayers::MOVING;
    }
    virtual uint GetNumBroadPhaseLayers() const override { return BroadPhaseLayers::NUM_LAYERS; }
    virtual BroadPhaseLayer GetBroadPhaseLayer(ObjectLayer inLayer) const override { return mObjectToBroadPhase[inLayer]; }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    virtual const char* GetBroadPhaseLayerName(BroadPhaseLayer inLayer) const override
    {
        switch ((BroadPhaseLayer::Type)inLayer)
        {
        case (BroadPhaseLayer::Type)BroadPhaseLayers::NON_MOVING: return "NON_MOVING";
        case (BroadPhaseLayer::Type)BroadPhaseLayers::MOVING:     return "MOVING";
        default:                                                  return "INVALID";
        }
    }
#endif
private:
    BroadPhaseLayer mObjectToBroadPhase[Layers::NUM_LAYERS];
};

class ObjectVsBroadPhaseLayerFilterImpl : public ObjectVsBroadPhaseLayerFilter
{
public:
    virtual bool ShouldCollide(ObjectLayer inLayer1, BroadPhaseLayer inLayer2) const override
    {
        switch (inLayer1)
        {
        case Layers::NON_MOVING:
            return inLayer2 == BroadPhaseLayers::MOVING;
        case Layers::MOVING:
            return true;
        default:
            return false;
        }
    }
};

namespace
{
    constexpr float kGroundExtent = 100.0f;
    constexpr float kGroundY = 0.0f;
    

    struct SimulationState
    {
        GeometryHandle ground;
        GeometryHandle box;
        std::vector<BodyID> bodies;
        BodyInterface* bodyInterface;
    };

    void CommitDemoScene(GPUScene& gpu, SimulationState const& state, RendererUBO& ubo)
    {
        uint32_t const numInstances = 1 + (uint32_t)state.bodies.size();
        auto tables = gpu.BeginScene(numInstances, 2, 2);
        
        tables.instances[0] = GSInstance{
            .transform = float3(0, 0, 0),
            .rotation = quat(0, 0, 0, 1),
            .scale = float3(1, 1, 1),
            .materialIndex = 0,
            .resourceIndex = state.ground.index,
            .type = kGSInstanceTypeMesh,
        };

        for (size_t i = 0; i < state.bodies.size(); ++i)
        {
            RVec3 pos = state.bodyInterface->GetCenterOfMassPosition(state.bodies[i]);
            Quat rot = state.bodyInterface->GetRotation(state.bodies[i]);

            tables.instances[1 + i] = GSInstance{
                .transform = float3(pos.GetX(), pos.GetY(), pos.GetZ()),
                .rotation = quat(rot.GetX(), rot.GetY(), rot.GetZ(), rot.GetW()),
                .scale = float3(1, 1, 1),
                .materialIndex = 1,
                .resourceIndex = state.box.index,
                .type = kGSInstanceTypeMesh,
            };
        }

        tables.materials[0] = GSMaterial{};
        tables.materials[0].baseColorFactor = float4(0.8f, 0.8f, 0.8f, 1.0f);
        tables.materials[0].metallicFactor = 0.0f;
        tables.materials[0].roughnessFactor = 0.9f;
        tables.materials[0].ior = 1.5f;

        tables.materials[1] = GSMaterial{};
        tables.materials[1].baseColorFactor = float4(0.2f, 0.6f, 0.9f, 1.0f);
        tables.materials[1].metallicFactor = 0.0f;
        tables.materials[1].roughnessFactor = 0.5f;
        tables.materials[1].ior = 1.5f;

        tables.lights[0] = GSLight{
            .flags = kGSLightTypeEnvironment,
            .color = float3(0.45f, 0.55f, 0.7f),
            .power = 1.0f,
        };
        tables.lights[1] = GSLight{.flags = kGSLightTypeDirectional | to_integer(GSLightFlagsBits::UseShadow),
                                   .color = float3(1.0f, 0.96f, 0.9f),
                                   .power = 2.0f,            
                                   .direction = float3(0.0f, -1.0f, 0.0f),
                                   .params = float4(.05f, 0.0f, 0.0f, 0.0f)};
        gpu.EndScene(tables);
        gpu.UpdateUBO(ubo);
    }

    void RebuildGraph(ExampleVulkanContext& ctx, RendererUBO& ubo, GPUScene& gpu, RendererConfig& cfg,
                      RendererOutputs& outputs, ExampleInputState& input, ExampleRenderer renderer)
    {
        Examples_ResetRenderer(ctx, RendererDesc{});
        ctx.renderer->BeginSetup();
        cfg.renderExtent = ctx.renderer->GetSwapchainExtent();
        ubo.ptMaxBounces = 1u;
        auto resources = CreateGPUSceneRendererResources(ctx.renderer.get(), &gpu);
        BuildGPUSceneHostUpdatePass(ctx.renderer.get(), resources);
        if (renderer == ExampleRenderer::PathTracer)
            Example_BuildExamplePathTracerRenderGraph(ctx.renderer.get(), &ubo, resources, cfg, outputs);
        else
            Example_BuildExampleRasterRenderGraph(ctx.renderer.get(), &ubo, resources, cfg, outputs);
        Examples_BuildTonemappingPass(ctx.renderer.get(), outputs, true);
        RenderUtils::createCSDebugTextPassBackBuffer(ctx.renderer.get(), "Debug Text", Examples_HudLines(input));
        ctx.renderer->EndSetup();
    }
}

int main(int argc, char** argv)
{
    RegisterDefaultAllocator();
    Factory::sInstance = new Factory();
    RegisterTypes();

    TempAllocatorImpl temp_allocator(10 * 1024 * 1024);
    
    const uint cMaxBodies = 1024;
    const uint cNumBodyMutexes = 0;
    const uint cMaxBodyPairs = 1024;
    const uint cMaxContactConstraints = 1024;

    BPLayerInterfaceImpl broad_phase_layer_interface;
    ObjectVsBroadPhaseLayerFilterImpl object_vs_broadphase_layer_filter;
    ObjectLayerPairFilterImpl object_vs_object_layer_filter;

    PhysicsSystem physics_system;
    physics_system.Init(cMaxBodies, cNumBodyMutexes, cMaxBodyPairs, cMaxContactConstraints, 
                        broad_phase_layer_interface, object_vs_broadphase_layer_filter, object_vs_object_layer_filter);

    BodyInterface& body_interface = physics_system.GetBodyInterface();
    
    BoxShapeSettings floor_shape_settings(Vec3(50.0f, 1.0f, 50.0f));
    floor_shape_settings.SetEmbedded();
    ShapeRefC floor_shape = floor_shape_settings.Create().Get();
    BodyCreationSettings floor_settings(floor_shape, RVec3(0.0_r, -1.0_r, 0.0_r), Quat::sIdentity(), EMotionType::Static, Layers::NON_MOVING);
    Body* floor = body_interface.CreateBody(floor_settings);
    body_interface.AddBody(floor->GetID(), EActivation::DontActivate);

    std::vector<BodyID> box_bodies;
    BoxShapeSettings box_shape_settings(Vec3(0.5f, 0.5f, 0.5f));
    box_shape_settings.SetEmbedded();
    ShapeRefC box_shape = box_shape_settings.Create().Get();

    for (int x = 0; x < 5; ++x) {
        for (int y = 0; y < 10; ++y) {
            for (int z = 0; z < 5; ++z) {
                BodyCreationSettings box_settings(box_shape, RVec3(x * 1.5f - 3.0f, y * 1.5f + 5.0f, z * 1.5f - 3.0f), Quat::sIdentity(), EMotionType::Dynamic, Layers::MOVING);
                BodyID box_id = body_interface.CreateAndAddBody(box_settings, EActivation::Activate);
                box_bodies.push_back(box_id);
            }
        }
    }
    physics_system.OptimizeBroadPhase();

    SDL_Window* window = SDL_CreateWindow(FOUNDATION_APPLICATION_TITLE("GPUScene Physics Jolt"), 1280, 720,
                                          Examples_SDLWindowFlagsVulkan);
    auto ctx = Examples_InitVulkan(window, argc, argv, RendererDesc{});
    Foundation::Examples::FoundationJoltJobSystem job_system(ctx.jobs.get(), cMaxPhysicsJobs, cMaxPhysicsBarriers);

    GPUSceneDesc desc{};
    desc.primitiveBudget = 64u * 1024u;
    desc.dynamicGeometryBudget = 256u * 1024u;
    desc.dynamicStagingBudget = 256u * 1024u;
    desc.instanceBudget = cMaxBodies * 2;
    desc.materialBudget = 8;
    desc.lightBudget = 8;
    desc.geometryBudget = 8;
    desc.tlasInstanceBudget = cMaxBodies * 2;
    GPUScene gpu(ctx.device.Get(), ctx.jobs.get(), GLOBAL_ALLOC, desc);
    
    SimulationState state;
    state.bodies = box_bodies;
    state.bodyInterface = &body_interface;

    {
        FImportedMesh groundMesh = Examples_MakePlaneMesh(kGroundExtent, kGroundY, GLOBAL_ALLOC);
        groundMesh.Optimize();
        groundMesh.ClusterizeDAG();
        gpu.Upload(groundMesh, state.ground);

        FImportedMesh boxMesh = Examples_MakeBoxMesh(1.0f, GLOBAL_ALLOC);
        boxMesh.Optimize();
        boxMesh.ClusterizeDAG();
        gpu.Upload(boxMesh, state.box);
        gpu.Join();
    }

    RendererUBO ubo{};
    RendererConfig cfg{.cullFlags{CullFlagsBits::Frustum | CullFlagsBits::Backface}};
    if (!ctx.device->GetCapabilities().raytracingInline)
        cfg.viewFlags &= ~ViewFlagsBits::EnableRasterRTShadows;

    FExampleOrbitCamera camera{.center = {0.0f, 5.0f, 0.0f},
                               .radius = 20.0f,
                               .rot = normalize(angleAxis(radians(-20.0f), float3(1, 0, 0))),
                               .zNear = 0.01f,
                               .fovY = radians(45.0f)};
    RendererOutputs outputs{};
    ExampleInputState input{};
    ExampleFpsCounter fps;
    float paused = 0.0f;
    ExampleRenderer renderer = ExampleRenderer::PathTracer;
    uint64_t t0 = SDL_GetTicksNS();
    float lastClickTime = -1.0f;
    const float kShootIntervalMS = 250;
    while (true)
    {
        uint64_t t1 = SDL_GetTicksNS();
        float dt = static_cast<float>(t1 - t0) / 1e9f;
        t0 = t1;
        Examples_BeginFrameInput(input);
        if (Examples_PollEvents(window, ctx, input))
            break;
            
        bool clicked = input.pointerDown && !input.mouseCapturedByHud && (t1 - lastClickTime > kShootIntervalMS * 1e6f);

        if (clicked && box_bodies.size() < cMaxBodies / 2 /* reserve.. */)
        {
            lastClickTime = t1;
            float ndcX = (input.clickPosition.x / (float)ctx.renderer->GetSwapchainExtent().x) * 2.0f - 1.0f;
            float ndcY = (1.0f - input.clickPosition.y / (float)ctx.renderer->GetSwapchainExtent().y) * 2.0f - 1.0f;
            mat4 invVP = inverse(camera.proj * camera.view);
            vec4 target = invVP * vec4(ndcX,ndcY, 1e-5f, 1.0f);
            target /= target.w;
            vec3 dir = normalize(vec3(target) - camera.position);
            
            BodyCreationSettings proj_settings(box_shape, RVec3(camera.position.x, camera.position.y, camera.position.z), Quat::sIdentity(), EMotionType::Dynamic, Layers::MOVING);
            BodyID proj_id = body_interface.CreateAndAddBody(proj_settings, EActivation::Activate);
            body_interface.SetLinearVelocity(proj_id, Vec3(dir.x, dir.y, dir.z) * 25.0f);
            
            box_bodies.push_back(proj_id);
            state.bodies.push_back(proj_id);
        }

        if (input.wantResizeOrRebuild)
        {
            input.wantResizeOrRebuild = false;
            RebuildGraph(ctx, ubo, gpu, cfg, outputs, input, renderer);
        }

        if (paused < 0.5f || clicked)
        {
            physics_system.Update(dt, 1, &temp_allocator, &job_system);
            ubo.ptAccumulatedFrames = 0u;
        }
        else
        {
            ubo.ptAccumulatedFrames += ubo.ptSamplesPerPixel;
        }

        if (camera.Update(input, dt))
            ubo.ptAccumulatedFrames = 0u;

        Examples_UpdateCameraUBO(ubo, ctx.renderer.get(), camera, cfg);
        CommitDemoScene(gpu, state, ubo);

        Examples_Text(input,
                      fmt::format("Jolt Physics | {:.0f} FPS | refit {} rebuild {}", fps.Update(),
                                  gpu.GetDynamicRefitCount(), gpu.GetDynamicRebuildCount()));
        Examples_Text(input, FExampleOrbitCamera::kControlsText);
        Examples_Slider(input, "Paused", paused, 0.0f, 1.0f, 1.0f, "");
        if (Examples_RendererSwitchButton(input, renderer))
            input.wantResizeOrRebuild = true;
        Examples_NewFrame(window, ctx);
    }

    Examples_DestroyVulkan(window, ctx);
    
    body_interface.RemoveBody(floor->GetID());
    body_interface.DestroyBody(floor->GetID());
    for (auto id : box_bodies)
    {
        body_interface.RemoveBody(id);
        body_interface.DestroyBody(id);
    }
    UnregisterTypes();
    delete Factory::sInstance;
    Factory::sInstance = nullptr;

    return 0;
}
