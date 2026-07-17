// Headless PathTracer - renders an authored still-life scene with the path tracer
// (no SDL window, no swapchain, no Vulkan WSI), accumulates 128 frames while reporting
// progress to stdout, then reads back the tonemapped framebuffer, writes it to render.png
// and opens it with the OS default viewer.
//
// Validates the headless (null swapchain) Renderer path end-to-end through GPUScene + the
// path-tracer render graph + the simplified tonemap pass, including cross-frame accumulation.
#include <Renderer/GPUScene.hpp>
#include <Renderer/Renderer.hpp> // RendererUBO + BuildPathTracerRenderGraph
#include <Editor/Scene/Mesh.hpp> // FImportedMesh / LoadObj
#include <RenderCore/ImmediateContext.hpp>
#include <Core/Paths.hpp>
#include "Examples.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>

using namespace Foundation;
using namespace Core;
using namespace RHI;
using namespace RenderCore;
using Foundation::Core::PathsResolve;

namespace
{
    constexpr RHIExtent2D kExtent{1024, 1024};
    constexpr uint32_t kAccumFrames = 2048;
    constexpr float kPi = glm::pi<float>();
    // Still-life Cornell-style box (open front + top).
    constexpr float boxW = 4.0f; // interior width  (x in [-2, 2])
    constexpr float boxH = 3.0f; // interior height (y in [ 0, 3])
    constexpr float boxD = 4.0f; // interior depth  (z in [-2, 2])
}

int main(int argc, char** argv)
{
    RendererDesc rendererDesc{.threadCount = 0};
    auto [renderer0, app, device, surface, swapchain, presenter] = Examples_InitVulkan(nullptr, argc, argv, rendererDesc);
    UniquePtr<Renderer> renderer(renderer0, StlDeleter<Renderer>{GLOBAL_ALLOC});

    {
        // GPUScene owns all GPU-resident scene data and lives for the example scope.
        GPUScene gpu(device.Get(), GLOBAL_ALLOC, GPUSceneDesc{});

        // Same pipeline the editor uses (optimize -> DAG clusterize -> quantize), serialized
        // into a throwaway blob payload and uploaded synchronously.
        auto UploadMesh = [&](FImportedMesh& mesh) -> GeometryHandle
        {
            mesh.Optimize();
            mesh.ClusterizeDAG();
            CHECK_MSG(mesh.EnsureQuantized(), "Failed to quantize mesh");

            Vector<unsigned char> payload(GLOBAL_ALLOC);
            MemoryBlobSerializer blobs(payload);
            FSerializedMesh serialized(GLOBAL_ALLOC);
            serialized.vertices = blobs.AppendArray(mesh.verticesQuantized);
            serialized.vertexCount = static_cast<uint32_t>(mesh.verticesQuantized.size());
            FSerializedMeshLOD& lod0 = serialized.lods.emplace_back();
            lod0.indices = blobs.AppendArray(mesh.lods[0].indices);
            lod0.indexCount = static_cast<uint32_t>(mesh.lods[0].indices.size());
            serialized.dagGroups = blobs.AppendArray(mesh.dag.groups);
            serialized.dagMeshlets = blobs.AppendArray(mesh.dag.meshlets);
            serialized.dagMeshletTri = blobs.AppendArray(mesh.dag.meshletTri);
            serialized.dagMeshletVtx = blobs.AppendArray(mesh.dag.meshletVtx);

            FBlobDeserializer deserializer = blobs.Deserializer();
            GeometryHandle handle;
            GPUScene::Result r = gpu.Upload(&deserializer, serialized, handle);
            CHECK_MSG(r == GPUScene::Result::InProgress, "Mesh upload rejected ({})", r);
            gpu.Join(); // Drain synchronously: geometry bytes resident + BLAS built.
            CHECK_MSG(gpu.Query(handle) == GPUScene::Result::Ready, "Mesh did not become resident");
            return handle;
        };

        // --- Geometry: the bunny + a unit XZ plane (instanced into the box walls) ------------
        float3 bunnyMin{1e30f, 1e30f, 1e30f};
        float3 bunnyMax{-1e30f, -1e30f, -1e30f};
        GeometryHandle bunny;
        {
            FImportedMesh mesh(GLOBAL_ALLOC);
            LoadObj(mesh, PathsResolve("Data/Assets/bunny.obj"));
            for (FVertex const& v : mesh.vertices)
            {
                bunnyMin.x = std::min(bunnyMin.x, v.position.x);
                bunnyMin.y = std::min(bunnyMin.y, v.position.y);
                bunnyMin.z = std::min(bunnyMin.z, v.position.z);
                bunnyMax.x = std::max(bunnyMax.x, v.position.x);
                bunnyMax.y = std::max(bunnyMax.y, v.position.y);
                bunnyMax.z = std::max(bunnyMax.z, v.position.z);
            }
            bunny = UploadMesh(mesh);
        }
        GeometryHandle plane;
        {
            constexpr int seg = 16;
            FImportedMesh mesh(GLOBAL_ALLOC);
            auto& lod = mesh.lods[0];
            for (int z = 0; z <= seg; ++z)
                for (int x = 0; x <= seg; ++x)
                {
                    FVertex v{};
                    v.position = float3(static_cast<float>(x) / seg - 0.5f, 0.0f,
                                        static_cast<float>(z) / seg - 0.5f);
                    v.normal = float3(0.0f, 1.0f, 0.0f);
                    v.tangent = float3(1.0f, 0.0f, 0.0f);
                    v.bitangentSign = 1.0f;
                    v.uv = float2(static_cast<float>(x) / seg, static_cast<float>(z) / seg);
                    mesh.vertices.push_back(v);
                }
            for (int z = 0; z < seg; ++z)
                for (int x = 0; x < seg; ++x)
                {
                    uint32_t i0 = static_cast<uint32_t>(z * (seg + 1) + x);
                    uint32_t i1 = i0 + 1u;
                    uint32_t i2 = i0 + static_cast<uint32_t>(seg + 1);
                    uint32_t i3 = i2 + 1u;
                    lod.indices.push_back(i0);
                    lod.indices.push_back(i2);
                    lod.indices.push_back(i1);
                    lod.indices.push_back(i1);
                    lod.indices.push_back(i2);
                    lod.indices.push_back(i3);
                }
            plane = UploadMesh(mesh);
        }

        // --- Material palette: walls + three hero BSDFs --------------------------------------
        RendererUBO ubo{.adaptiveThreshold = 0.1f};
        auto BaseMat = []
        {
            GSMaterial m{};
            m.baseColorFactor = float4(0.8f, 0.8f, 0.8f, 1.0f);
            m.metallicFactor = 0.0f;
            m.roughnessFactor = 0.5f;
            m.ior = 1.5f;
            m.specularFactor = 1.0f;
            m.specularColorFactor = float3(1.0f);
            m.clearcoatRoughnessFactor = 0.04f;
            m.shaderBlockID = 0u; // Principled
            return m;
        };
        Vector<GSMaterial> palette(GLOBAL_ALLOC);
        const uint32_t matWhite = static_cast<uint32_t>(palette.size());
        {
            GSMaterial m = BaseMat();
            m.baseColorFactor = float4(0.725f, 0.71f, 0.68f, 1.0f);
            m.roughnessFactor = 1.0f;
            palette.push_back(m);
        }
        const uint32_t matRed = static_cast<uint32_t>(palette.size());
        {
            GSMaterial m = BaseMat();
            m.baseColorFactor = float4(0.63f, 0.065f, 0.05f, 1.0f);
            m.roughnessFactor = 1.0f;
            palette.push_back(m);
        }
        const uint32_t matGreen = static_cast<uint32_t>(palette.size());
        {
            GSMaterial m = BaseMat();
            m.baseColorFactor = float4(0.14f, 0.45f, 0.091f, 1.0f);
            m.roughnessFactor = 1.0f;
            palette.push_back(m);
        }
        // Heroes: glass, gold, and a diffuse blue lacquer.
        const uint32_t matGlass = static_cast<uint32_t>(palette.size());
        {
            GSMaterial m = BaseMat();
            m.baseColorFactor = float4(0.92f, 0.96f, 1.0f, 1.0f);
            m.roughnessFactor = 0.02f;
            m.transmissionFactor = 1.0f;
            m.ior = 1.5f;
            palette.push_back(m);
        }
        const uint32_t matGold = static_cast<uint32_t>(palette.size());
        {
            GSMaterial m = BaseMat();
            m.baseColorFactor = float4(1.0f, 0.84f, 0.40f, 1.0f);
            m.metallicFactor = 1.0f;
            m.roughnessFactor = 0.05f;
            palette.push_back(m);
        }
        const uint32_t matBlue = static_cast<uint32_t>(palette.size());
        {
            GSMaterial m = BaseMat();
            m.baseColorFactor = float4(0.03f, 0.06f, 0.45f, 1.0f);
            m.roughnessFactor = 0.5f;
            m.clearcoatFactor = 1.0f;
            m.clearcoatRoughnessFactor = 0.04f;
            palette.push_back(m);
        }

        // Three bunnies in a row, scaled to a fixed height and stood on the floor.
        const float3 bunnyCenter = (bunnyMin + bunnyMax) * 0.5f;
        const float bunnyScale = 0.55f / std::max(bunnyMax.y - bunnyMin.y, 1e-4f);
        const uint32_t heroMats[] = {matGlass, matGold, matBlue};
        constexpr int kHeroes = static_cast<int>(sizeof(heroMats) / sizeof(heroMats[0]));
        const float heroSpacing = 1.1f;

        const uint32_t instanceCount = 4u + static_cast<uint32_t>(kHeroes); // 4 walls + 3 bunnies
        const uint32_t lightCount = 2u;                                     // env + rect key

        // Author the static still-life once (no animation: the camera and scene are fixed so the
        // path tracer accumulates a single converged image).
        auto AuthorScene = [&]
        {
            auto tables = gpu.BeginScene(instanceCount, static_cast<uint32_t>(palette.size()), lightCount);
            uint32_t idx = 0u;
            auto AddWall = [&](quat rot, float3 scale, float3 pos, uint32_t mat)
            {
                tables.instances[idx++] = InstanceDesc{.geometry = plane, .transform = pos, .rotation = rot,
                                                        .scale = scale, .materialIndex = mat};
            };
            AddWall(angleAxis(0.0f, float3(1, 0, 0)), float3(boxW, 1.0f, boxD), float3(0.0f, 0.0f, 0.0f),
                    matWhite); // floor
            AddWall(angleAxis(kPi * 0.5f, float3(1, 0, 0)), float3(boxW, boxH, 1.0f),
                    float3(0.0f, boxH * 0.5f, -boxD * 0.5f), matWhite); // back
            AddWall(angleAxis(-kPi * 0.5f, float3(0, 0, 1)), float3(1.0f, boxH, boxD),
                    float3(-boxW * 0.5f, boxH * 0.5f, 0.0f), matRed); // left
            AddWall(angleAxis(kPi * 0.5f, float3(0, 0, 1)), float3(1.0f, boxH, boxD),
                    float3(boxW * 0.5f, boxH * 0.5f, 0.0f), matGreen); // right

            for (int i = 0; i < kHeroes; ++i)
            {
                float px = (static_cast<float>(i) - (kHeroes - 1) * 0.5f) * heroSpacing;
                // Stand the bunny on the floor: its world-space pivot sits at the model's min-Y.
                tables.instances[idx++] = InstanceDesc{
                    .geometry = bunny,
                    .transform = float3(px - bunnyCenter.x * bunnyScale, -bunnyMin.y * bunnyScale, 0.0f),
                    .rotation = angleAxis(0.0f, float3(0, 1, 0)),
                    .scale = float3(bunnyScale),
                    .materialIndex = heroMats[i]};
            }

            for (size_t i = 0; i < palette.size(); ++i)
                tables.materials[i] = palette[i];

            // Single soft rectangular overhead area light (1.4 x 1.4 quad facing straight down).
            GSLight& env = tables.lights[0];
            env = GSLight{};
            env.flags = kGSLightTypeEnvironment;
            env.color = float3(0.0f);
            env.power = 1.0f;

            GSLight& key = tables.lights[1];
            key = GSLight{};
            key.flags = kGSLightTypeRect | kGSLightFlagUseShadow;
            key.color = float3(1.0f, 0.95f, 0.88f);
            key.power = 10.0f;
            key.position = float3(0.0f, boxH - 0.02f, 0.0f);
            key.dpdu = float3(0.70f, 0.0f, 0.0f);
            key.dpdv = float3(0.0f, 0.0f, 0.70f);
            key.direction = float3(0.0f, -1.0f, 0.0f);

            gpu.EndScene(tables, ubo.frameNumber);
            gpu.BuildUBO(ubo);
        };
        AuthorScene();

        ubo.ptSamplesPerPixel = 1;

        // --- One-time TLAS build -------------------------------------------------------------
        {
            ImmediateContext ctx(RHIDeviceQueueType::Compute, device.Get());
            auto* cmd = ctx.Get();
            cmd->Begin();
            auto tlasResult = gpu.BuildTLAS(cmd, /*update*/ false);
            cmd->End();
            if (tlasResult == GPUScene::TLASBuildResult::Built)
                ctx.Submit(), ctx.WaitIdle();
        }

        // --- Build the render graph (path tracer -> simplified tonemap into an RTV) ----------
        bool renderPaused = false; // PT dispatch gate (never paused: trace every frame)
        RendererConfig cfg{};
        cfg.renderExtent = kExtent;
        cfg.ptRenderPaused = &renderPaused;
        RendererOutputs handles{};
        renderer->BeginSetup();
        BuildPathTracerRenderGraph(renderer.get(), &ubo, &gpu, cfg, handles);
        // Headless: the tonemap writes only to the explicit RTV (no backbuffer blit).
        // We capture that handle to read it back after convergence.
        const ResourceHandle postprocess = Examples_InsertBasicTonemapPasses(renderer.get(), handles, true);
        renderer->EndSetup();

        // --- Fixed camera: look into the open front of the box from slightly above ----------
        FExampleOrbitCamera camera{.center = {0.0f, 0.9f, 0.0f},
                                   .radius = 5.2f,
                                   .rot = normalize(angleAxis(radians(-6.0f), float3(1, 0, 0))),
                                   .zNear = 0.01f,
                                   .fovY = radians(45.0f)};
        camera.aspect = static_cast<float>(kExtent.x) / static_cast<float>(kExtent.y);
        camera.RefreshMatrices();

        auto UpdateUBO = [&]
        {
            Examples_GPUSceneFillCameraUBO(ubo, renderer.get(), camera, cfg);
        };
        UpdateUBO();

        // --- Accumulation loop ---------------------------------------------------------------
        // Scene + camera are static, so ptAccumulatedFrames never resets and the path tracer
        // converges one accumulated sample per frame. Report progress to stdout each frame.
        fmt::println("HeadlessPathTracer: accumulating {} frames ({}x{}, {} spp/frame)",
                     kAccumFrames, kExtent.x, kExtent.y, ubo.ptSamplesPerPixel);
        for (uint32_t f = 0; f < kAccumFrames; ++f)
        {
            UpdateUBO(); // refresh frameNumber each frame; camera/scene unchanged.
            Examples_NewFrame(renderer.get());
            ubo.ptAccumulatedFrames += ubo.ptSamplesPerPixel;

            fmt::print("\r[PT] frame {}/{} ({:.0f}%)   ", f + 1, kAccumFrames,
                       100.0f * static_cast<float>(f + 1) / static_cast<float>(kAccumFrames));
            std::fflush(stdout);
        }
        fmt::println("");
        LOG(HeadlessPathTracer, LogInfo, "Accumulation complete ({} frames)", kAccumFrames);

        // --- Drain the last frame before readback -------------------------------------------
        renderer->WaitForPreviousFrame();
        device->WaitIdle();

        // --- Read back the tonemapped RTV and dump + open it --------------------------------
        // Scoped so the readback (which owns device resources) is destroyed before the
        // GPUScene / device teardown below.
        {
            auto* outputTex = renderer->DerefResource(postprocess).Get<RHITexture*>();
            const size_t dataSize = static_cast<size_t>(kExtent.x) * kExtent.y * 4;
            ImmediateReadback readback(device.Get(), dataSize + 16);
            readback.Begin();
            {
                auto* cmd = readback.ctx.Get();
                cmd->BeginTransition();
                cmd->SetImageTransition(outputTex,
                    {.srcAccess = RHIResourceAccessBits::RenderTargetWrite,
                     .dstAccess = RHIResourceAccessBits::TransferRead,
                     .srcStage = RHIPipelineStageBits::RenderTargetOutput,
                     .dstStage = RHIPipelineStageBits::Transfer,
                     .srcImgLayout = RHITextureLayout::RenderTarget,
                     .dstImgLayout = RHITextureLayout::TransferSrc,
                     .srcImgRange = RHITextureSubresourceRange::Create()});
                cmd->EndTransition();
            }
            char* pixels = readback.Readback(outputTex, dataSize,
                                             RHITextureSubresourceLayer{.aspect = RHITextureAspectFlagBits::Color},
                                             RHIOffset2D{}, kExtent);
            readback.End();
            readback.WaitIdle();

            CHECK_MSG(pixels, "Headless PT readback failed (out of staging memory)");
            // Dump the converged framebuffer to render.png and open it with the OS default viewer.
            Examples_DumpAndOpenImage("render.png", kExtent, pixels);
        }

        device->WaitIdle();
        // GPUScene tears down here (end of scope); renderer/app/device tear down below.
    }

    Examples_DestroyVulkan(nullptr, renderer.release(), app, device, surface, swapchain);
    fmt::println("HeadlessPathTracer: OK");
    return 0;
}
