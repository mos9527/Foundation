// Box Galore(tm)
// Example demonstrating integartion with Jolt Physics!
#include "Examples.hpp"
#include "Jolt/JoltGPUScene.hpp"

#include <Renderer/Mesh.hpp>
#include <Renderer/Rasterizer.hpp>
#include <Renderer/ProgressivePathtracer.hpp>
#include <cmath>

using namespace Foundation::Examples;

namespace
{
    constexpr float kGroundExtent = 100.0f;
    constexpr float kGroundY = 0.0f;
    constexpr uint32_t kMaxBodies = 1024;

    PhysicsRayDesc PointerRay(float2 pointer, uint2 extent, FExampleOrbitCamera const& camera)
    {
        float const ndcX = pointer.x / static_cast<float>(extent.x) * 2.0f - 1.0f;
        float const ndcY = (1.0f - pointer.y / static_cast<float>(extent.y)) * 2.0f - 1.0f;
        mat4 const invVP = inverse(camera.proj * camera.view);
        vec4 target = invVP * vec4(ndcX, ndcY, 1e-5f, 1.0f);
        target /= target.w;
        return PhysicsRayDesc{
            .origin = camera.position,
            .direction = normalize(vec3(target) - camera.position),
            .maxDistance = 1000.0f,
            .layerMask = 1u << kPhysicsLayerMoving,
        };
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
        Example_BuildExampleRenderer(renderer, ctx.renderer.get(), &ubo, resources, cfg, outputs);
        Examples_BuildTonemappingPass(ctx.renderer.get(), &ubo, outputs, true);
        RenderUtils::createCSDebugTextPassBackBuffer(ctx.renderer.get(), "Debug Text", Examples_HudLines(input));
        ctx.renderer->EndSetup();
    }
} // namespace

int main(int argc, char** argv)
{
    SDL_Window* window = SDL_CreateWindow(FOUNDATION_APPLICATION_TITLE("GPUScene Physics Jolt"), 1280, 720,
                                          Examples_SDLWindowFlagsVulkan);
    auto ctx = Examples_InitVulkan(window, argc, argv, RendererDesc{});

    {
        JoltGPUSceneDesc desc{};
        desc.physics.maxBodies = kMaxBodies;
        desc.gpu.primitiveBudget = 64u * 1024u;
        desc.gpu.dynamicGeometryBudget = 256u * 1024u;
        desc.gpu.dynamicStagingBudget = 256u * 1024u;
        desc.gpu.instanceBudget = kMaxBodies * 2;
        desc.gpu.materialBudget = 8;
        desc.gpu.lightBudget = 8;
        desc.gpu.geometryBudget = 8;
        desc.gpu.tlasInstanceBudget = kMaxBodies * 2;
        JoltGPUScene scene(ctx.device.Get(), ctx.jobs.get(), desc);

        PhysicsShapeHandle floorShape{};
        PhysicsShapeHandle boxShape{};
        CHECK(scene.CreateShape(
                  PhysicsShapeDesc{.type = PhysicsShapeType::Box, .halfExtent = float3(50.0f, 1.0f, 50.0f)},
                  floorShape) == PhysicsStatus::Ok);
        CHECK(scene.CreateShape(
                  PhysicsShapeDesc{.type = PhysicsShapeType::Box, .halfExtent = float3(0.5f, 0.5f, 0.5f)},
                  boxShape) == PhysicsStatus::Ok);

        GeometryHandle groundGeometry{};
        GeometryHandle boxGeometry{};
        {
            FImportedMesh groundMesh = Examples_MakePlaneMesh(kGroundExtent, kGroundY, GLOBAL_ALLOC);
            groundMesh.Optimize();
            groundMesh.ClusterizeDAG();
            CHECK(scene.Upload(groundMesh, groundGeometry) == GPUScene::Result::Ready);

            FImportedMesh boxMesh = Examples_MakeBoxMesh(1.0f, GLOBAL_ALLOC);
            boxMesh.Optimize();
            boxMesh.ClusterizeDAG();
            CHECK(scene.Upload(boxMesh, boxGeometry) == GPUScene::Result::Ready);
            scene.Join();
        }

        GSMaterial groundMaterial{};
        groundMaterial.baseColorFactor = float4(0.8f, 0.8f, 0.8f, 1.0f);
        groundMaterial.metallicFactor = 0.0f;
        groundMaterial.roughnessFactor = 0.9f;
        groundMaterial.ior = 1.5f;
        GPUSceneMaterialHandle groundMaterialHandle{};
        CHECK(scene.CreateMaterial(groundMaterial, groundMaterialHandle) == PhysicsStatus::Ok);

        GSMaterial boxMaterial{};
        boxMaterial.baseColorFactor = float4(0.2f, 0.6f, 0.9f, 1.0f);
        boxMaterial.metallicFactor = 0.0f;
        boxMaterial.roughnessFactor = 0.5f;
        boxMaterial.ior = 1.5f;
        GPUSceneMaterialHandle boxMaterialHandle{};
        CHECK(scene.CreateMaterial(boxMaterial, boxMaterialHandle) == PhysicsStatus::Ok);

        GSMaterial heldMaterial = boxMaterial;
        heldMaterial.baseColorFactor = float4(1.0f, 0.25f, 0.08f, 1.0f);
        heldMaterial.emissiveFactor = float3(0.2f, 0.02f, 0.0f);
        GPUSceneMaterialHandle heldMaterialHandle{};
        CHECK(scene.CreateMaterial(heldMaterial, heldMaterialHandle) == PhysicsStatus::Ok);

        GPUSceneLightHandle environmentLight{};
        CHECK(scene.CreateLight(GSLight{
                                    .flags = kGSLightTypeEnvironment,
                                    .color = float3(0.45f, 0.55f, 0.7f),
                                    .power = 1.0f,
                                },
                                environmentLight) == PhysicsStatus::Ok);
        GPUSceneLightHandle directionalLight{};
        CHECK(scene.CreateLight(GSLight{.flags = kGSLightTypeDirectional | to_integer(GSLightFlagsBits::UseShadow),
                                        .color = float3(1.0f, 0.96f, 0.9f),
                                        .power = 2.0f,
                                        .direction = float3(0.0f, -1.0f, 0.0f),
                                        .params = float4(.05f, 0.0f, 0.0f, 0.0f)},
                                directionalLight) == PhysicsStatus::Ok);

        PhysicsBodyHandle floorBody{};
        CHECK(scene.Spawn(PhysicsBodyDesc{.shape = floorShape,
                                          .pose = {.position = float3(0.0f, -1.0f, 0.0f)},
                                          .motion = PhysicsMotionType::Static,
                                          .layer = kPhysicsLayerNonMoving,
                                          .activation = PhysicsActivation::DontActivate},
                          PhysicsVisualDesc{.geometry = groundGeometry,
                                            .material = groundMaterialHandle,
                                            .localPose = {.position = float3(0.0f, 1.0f, 0.0f)}},
                          floorBody) == PhysicsStatus::Ok);

        for (int x = 0; x < 5; ++x)
        {
            for (int y = 0; y < 10; ++y)
            {
                for (int z = 0; z < 5; ++z)
                {
                    PhysicsBodyHandle body{};
                    CHECK(scene.Spawn(PhysicsBodyDesc{.shape = boxShape,
                                                      .pose = {.position = float3(x * 1.5f - 3.0f, y * 1.5f + 5.0f,
                                                                                  z * 1.5f - 3.0f)},
                                                      .motion = PhysicsMotionType::Dynamic,
                                                      .layer = kPhysicsLayerMoving},
                                      PhysicsVisualDesc{.geometry = boxGeometry, .material = boxMaterialHandle},
                                      body) == PhysicsStatus::Ok);
                }
            }
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
        ExampleRenderer renderer = ExampleRenderer::RealtimePT;
        uint64_t t0 = SDL_GetTicksNS();
        PhysicsBodyHandle heldBody{};
        PhysicsBodyPose heldPose{};
        float3 grabOffset{};
        float grabDistance = 0.0f;
        float3 previousGrabTarget{};
        float3 releaseVelocity{};
        uint64_t lastDragMoveTime = 0;
        bool wasPointerDown = false;
        while (true)
        {
            uint64_t t1 = SDL_GetTicksNS();
            float dt = static_cast<float>(t1 - t0) / 1e9f;
            t0 = t1;
            Examples_BeginFrameInput(input);
            if (Examples_PollEvents(window, ctx, input))
                break;

            bool const pointerPressed = input.pointerDown && !wasPointerDown && !input.mouseCapturedByHud;
            bool const pointerReleased = !input.pointerDown && wasPointerDown;
            if (pointerReleased && heldBody.IsValid())
            {
                // Release with inertia based on velocity
                float const idleSeconds = static_cast<float>(t1 - lastDragMoveTime) / 1e9f;
                float const releaseScale = std::exp(-8.0f * idleSeconds);
                CHECK(scene.SetLinearVelocity(heldBody, releaseVelocity * releaseScale) == PhysicsStatus::Ok);
                CHECK(scene.SetObjectMaterial(heldBody, boxMaterialHandle) == PhysicsStatus::Ok);
                heldBody = {};
            }

            if (pointerPressed)
            {
                PhysicsRayDesc const ray = PointerRay(input.clickPosition, ctx.renderer->GetSwapchainExtent(), camera);
                PhysicsRayHit hit{};
                if (scene.RayCast(ray, hit) == PhysicsStatus::Ok)
                {
                    heldBody = hit.body;
                    CHECK(scene.GetBodyPose(heldBody, heldPose) == PhysicsStatus::Ok);
                    grabDistance = hit.distance;
                    grabOffset = heldPose.position - hit.position;
                    previousGrabTarget = heldPose.position;
                    releaseVelocity = {};
                    lastDragMoveTime = t1;
                    CHECK(scene.SetObjectMaterial(heldBody, heldMaterialHandle) == PhysicsStatus::Ok);
                }
            }
            // No camera movements when held
            if (heldBody.IsValid())
                input.orbitDelta = {};
            wasPointerDown = input.pointerDown;

            if (input.wantResizeOrRebuild)
            {
                input.wantResizeOrRebuild = false;
                RebuildGraph(ctx, ubo, scene.GetGPUScene(), cfg, outputs, input, renderer);
            }

            if (paused < 0.5f || heldBody.IsValid())
            {
                CHECK(scene.Step(std::min(dt, 1.0f / 60.0f)) == PhysicsStatus::Ok);
                ubo.ptAccumulatedFrames = 0u;
            }
            else
            {
                ubo.ptAccumulatedFrames += ubo.ptSamplesPerPixel;
            }

            if (heldBody.IsValid() && input.pointerDown)
            {
                PhysicsRayDesc const ray = PointerRay(input.pointerPosition, ctx.renderer->GetSwapchainExtent(), camera);
                float3 const target = ray.origin + ray.direction * grabDistance + grabOffset;
                float3 const targetDelta = target - previousGrabTarget;
                if (dot(targetDelta, targetDelta) > 1e-8f)
                {
                    float const elapsed = std::max(static_cast<float>(t1 - lastDragMoveTime) / 1e9f, 1.0f / 240.0f);
                    float3 instantVelocity = targetDelta / elapsed;
                    float const speed = length(instantVelocity);
                    constexpr float kMaxThrowSpeed = 30.0f;
                    if (speed > kMaxThrowSpeed)
                        instantVelocity *= kMaxThrowSpeed / speed;
                    releaseVelocity = mix(releaseVelocity, instantVelocity, 0.5f);
                    previousGrabTarget = target;
                    lastDragMoveTime = t1;
                }
                heldPose.position = target;
                CHECK(scene.SetBodyPose(heldBody, heldPose) == PhysicsStatus::Ok);
                CHECK(scene.SetLinearVelocity(heldBody, releaseVelocity) == PhysicsStatus::Ok);
            }

            if (camera.Update(input, dt))
                ubo.ptAccumulatedFrames = 0u;

            Examples_UpdateCameraUBO(ubo, ctx.renderer.get(), camera, cfg);
            CHECK(scene.Commit(ubo) == PhysicsStatus::Ok);

            GPUScene& gpu = scene.GetGPUScene();
            Examples_Text(input,
                          Format("Jolt Physics | {:.0f} FPS | refit {} rebuild {}", fps.Update(),
                                      gpu.GetDynamicRefitCount(), gpu.GetDynamicRebuildCount()));
            Examples_Text(input, "Drag boxes | right-drag pan | pinch/wheel zoom | WASD move");
            Examples_Slider(input, "Paused", paused, 0.0f, 1.0f, 1.0f, "");
            if (Examples_RendererSwitchButton(input, renderer))
                input.wantResizeOrRebuild = true;
            if (Examples_RendererFlagsControls(input, renderer, cfg))
            {
                input.wantResizeOrRebuild = true;
                ubo.ptAccumulatedFrames = 0u;
            }
            Examples_NewFrame(window, ctx);
        }
    }

    Examples_DestroyVulkan(window, ctx);
    return 0;
}
