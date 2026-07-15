// Demonstrates asynchronous streaming with GPUScene
#include <Renderer/GPUScene.hpp>
#include <Renderer/Renderer.hpp> // UBO + Build{PathTracer,Raster}RenderGraph + Renderer{Scene,Config,Handles}
#include <Editor/Scene/Mesh.hpp> // FImportedMesh / FSerializedMesh / MemoryBlobSerializer
#include <Core/Paths.hpp>
#include "Examples.hpp"
#include <algorithm>
#include <cmath>

using Foundation::Core::PathsResolve;

namespace
{
constexpr float kPi = 3.14159265358979f;

// A deterministic "blob": a UV sphere whose radius is perturbed by a per-asset wave, so every
// asset is visually distinct and carries a non-trivial BLAS to build. Tessellation and
// displacement are seeded by the asset index so the stream looks varied.
FImportedMesh MakeBlob(Allocator* alloc, uint32_t seed)
{
    auto hash = [](uint32_t x) { x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16; return x; };
    const uint32_t h = hash(seed);
    const int rings = 28 + static_cast<int>(h % 16);        // 28..43
    const int sectors = 40 + static_cast<int>((h >> 8) % 24); // 40..63
    const float amp = 0.06f + 0.16f * ((h >> 16 & 0xff) / 255.0f);
    const float freqA = 2.0f + static_cast<float>(h % 5);
    const float freqB = 2.0f + static_cast<float>((h >> 4) % 5);

    FImportedMesh mesh(alloc);
    auto& lod = mesh.lods[0];
    for (int r = 0; r <= rings; ++r)
        for (int s = 0; s <= sectors; ++s)
        {
            float v = static_cast<float>(r) / rings * kPi;        // polar 0..pi
            float u = static_cast<float>(s) / sectors * 2.0f * kPi; // azimuth 0..2pi
            float3 dir = float3(std::sin(v) * std::cos(u), std::cos(v), std::sin(v) * std::sin(u));
            float disp = 1.0f + amp * std::sin(freqA * v + seed) * std::sin(freqB * u);
            FVertex vert{};
            vert.position = dir * (0.5f * disp);
            vert.uv = float2(static_cast<float>(s) / sectors, static_cast<float>(r) / rings);
            mesh.vertices.push_back(vert);
        }
    auto VID = [&](int r, int s) { return static_cast<uint32_t>(r * (sectors + 1) + s); };
    for (int r = 0; r < rings; ++r)
        for (int s = 0; s < sectors; ++s)
        {
            uint32_t i0 = VID(r, s), i1 = VID(r, s + 1), i2 = VID(r + 1, s), i3 = VID(r + 1, s + 1);
            lod.indices.push_back(i0); lod.indices.push_back(i2); lod.indices.push_back(i1);
            lod.indices.push_back(i1); lod.indices.push_back(i2); lod.indices.push_back(i3);
        }

    // Smooth normals from accumulated face normals (the displacement bends them off the sphere),
    // plus an arbitrary perpendicular tangent so the principled BSDF has a valid frame.
    Vector<float3> normals(mesh.vertices.size(), float3(0.0f), alloc);
    for (size_t i = 0; i + 2 < lod.indices.size(); i += 3)
    {
        uint32_t a = lod.indices[i], b = lod.indices[i + 1], c = lod.indices[i + 2];
        float3 fn = cross(mesh.vertices[b].position - mesh.vertices[a].position,
                          mesh.vertices[c].position - mesh.vertices[a].position);
        normals[a] += fn; normals[b] += fn; normals[c] += fn;
    }
    for (size_t i = 0; i < mesh.vertices.size(); ++i)
    {
        float3 n = normalize(normals[i] + float3(1e-6f, 1e-6f, 1e-6f));
        float3 ref = std::abs(n.y) < 0.99f ? float3(0, 1, 0) : float3(1, 0, 0);
        mesh.vertices[i].normal = n;
        mesh.vertices[i].tangent = normalize(cross(ref, n));
        mesh.vertices[i].bitangentSign = 1.0f;
    }
    return mesh;
}

// A unit XZ plane (+Y normal), lightly tessellated, used as the ground.
FImportedMesh MakePlane(Allocator* alloc, int seg = 8)
{
    FImportedMesh mesh(alloc);
    auto& lod = mesh.lods[0];
    for (int z = 0; z <= seg; ++z)
        for (int x = 0; x <= seg; ++x)
        {
            FVertex v{};
            v.position = float3(static_cast<float>(x) / seg - 0.5f, 0.0f, static_cast<float>(z) / seg - 0.5f);
            v.normal = float3(0, 1, 0);
            v.tangent = float3(1, 0, 0);
            v.bitangentSign = 1.0f;
            v.uv = float2(static_cast<float>(x) / seg, static_cast<float>(z) / seg);
            mesh.vertices.push_back(v);
        }
    for (int z = 0; z < seg; ++z)
        for (int x = 0; x < seg; ++x)
        {
            uint32_t i0 = static_cast<uint32_t>(z * (seg + 1) + x), i1 = i0 + 1u;
            uint32_t i2 = i0 + static_cast<uint32_t>(seg + 1), i3 = i2 + 1u;
            lod.indices.push_back(i0); lod.indices.push_back(i2); lod.indices.push_back(i1);
            lod.indices.push_back(i1); lod.indices.push_back(i2); lod.indices.push_back(i3);
        }
    return mesh;
}

// One streamed asset: its serialized payload + headers must outlive the background drain
// (GPUScene keeps pointers into them until the resource is Ready), so they live here for the
// program's lifetime. Stored in a reserved vector so their addresses never move.
struct StreamAsset
{
    Vector<unsigned char> payload{GLOBAL_ALLOC};
    FSerializedMesh serialized{GLOBAL_ALLOC};
    GeometryHandle handle{};
    uint32_t material{0};
};
} // namespace

int main(int argc, char** argv)
{
    SDL_Window* window = SDL_CreateWindow(FOUNDATION_APPLICATION_TITLE("GPUScene Async Streaming"), 1280, 720,
                                          Examples_SDLWindowFlagsVulkan);
    RendererDesc rendererDesc{};
    auto [renderer0, app, device, swapchain] = Examples_InitVulkan(window, argc, argv, rendererDesc);
    UniquePtr<Renderer> renderer(renderer0, StlDeleter<Renderer>{GLOBAL_ALLOC});

    constexpr uint32_t kBlobCount = 96u; // total procedural meshes that can be streamed in
    constexpr uint32_t kSpawnPerPress = 8u; // meshes enqueued per SPACE press

    // Budgets sized so nothing reallocates mid-stream: the TLAS stays in its pre-allocated 16 MB
    // buffer (tlasBudget) for the whole run, so its device AS object is stable and the render
    // graph never needs rebuilding as the instance count grows.
    {
    }
    GPUSceneDesc desc{};
    desc.primitiveBudget = 256u * (1u << 20); // headroom for kBlobCount procedural meshes
    desc.instanceBudget = 512u;
    desc.tlasInstanceBudget = 512u;
    desc.geometryBudget = kBlobCount + 4u;
    desc.materialBudget = 64u;
    desc.lightBudget = 16u;
    // desc.tlasBudget stays at its 16 MB default - the pre-allocation we rely on.
    {
        GPUScene gpu(device.Get(), GLOBAL_ALLOC, desc);

        // Serializes a CPU mesh into `a` (in place, so its addresses are stable) and enqueues the
        // upload. Crucially this does NOT Join: it returns as soon as the work is queued, and the
        // background worker uploads + builds the BLAS while we keep rendering.
        auto EnqueueAsync = [&](StreamAsset& a, FImportedMesh& mesh)
        {
            mesh.Optimize();
            mesh.ClusterizeDAG();
            CHECK_MSG(mesh.EnsureQuantized(), "Failed to quantize mesh");

            MemoryBlobSerializer blobs(a.payload);
            a.serialized = FSerializedMesh(GLOBAL_ALLOC);
            a.serialized.vertices = blobs.AppendArray(mesh.verticesQuantized);
            a.serialized.vertexCount = static_cast<uint32_t>(mesh.verticesQuantized.size());
            FSerializedMeshLOD& lod0 = a.serialized.lods.emplace_back();
            lod0.indices = blobs.AppendArray(mesh.lods[0].indices);
            lod0.indexCount = static_cast<uint32_t>(mesh.lods[0].indices.size());
            a.serialized.dagGroups = blobs.AppendArray(mesh.dag.groups);
            a.serialized.dagMeshlets = blobs.AppendArray(mesh.dag.meshlets);
            a.serialized.dagMeshletTri = blobs.AppendArray(mesh.dag.meshletTri);
            a.serialized.dagMeshletVtx = blobs.AppendArray(mesh.dag.meshletVtx);

            FBlobDeserializer des = blobs.Deserializer();
            GPUScene::Result r = gpu.Upload(&des, a.serialized, a.handle);
            CHECK_MSG(r == GPUScene::Result::InProgress, "Mesh upload rejected ({})", r);
        };

        // --- Material palette (all principled; each leans on a different part of the BSDF) --------
        auto BaseMat = []
        {
            GSMaterial m{};
            m.baseColorFactor = float4(0.8f, 0.8f, 0.8f, 1.0f);
            m.roughnessFactor = 0.5f;
            m.ior = 1.5f;
            m.specularFactor = 1.0f;
            m.specularColorFactor = float3(1.0f);
            m.clearcoatRoughnessFactor = 0.04f;
            m.shaderBlockID = 0u;
            return m;
        };
        Vector<GSMaterial> palette(GLOBAL_ALLOC);
        const uint32_t matFloor = static_cast<uint32_t>(palette.size());
        {
            GSMaterial m = BaseMat();
            m.baseColorFactor = float4(0.62f, 0.62f, 0.64f, 1.0f);
            m.roughnessFactor = 0.9f;
            palette.push_back(m);
        }
        {
            GSMaterial m = BaseMat();
            m.baseColorFactor = float4(0.85f, 0.25f, 0.20f, 1.0f);
            m.roughnessFactor = 0.55f;
            palette.push_back(m);
        }
        {
            GSMaterial m = BaseMat();
            m.baseColorFactor = float4(0.95f, 0.78f, 0.36f, 1.0f);
            m.metallicFactor = 1.0f;
            m.roughnessFactor = 0.18f;
            palette.push_back(m);
        }
        {
            GSMaterial m = BaseMat();
            m.baseColorFactor = float4(0.20f, 0.55f, 0.85f, 1.0f);
            m.roughnessFactor = 0.35f;
            palette.push_back(m);
        }
        {
            GSMaterial m = BaseMat();
            m.baseColorFactor = float4(0.92f, 0.96f, 1.0f, 1.0f);
            m.roughnessFactor = 0.03f;
            m.transmissionFactor = 1.0f;
            palette.push_back(m);
        }
        {
            GSMaterial m = BaseMat();
            m.baseColorFactor = float4(0.30f, 0.78f, 0.40f, 1.0f);
            m.metallicFactor = 1.0f;
            m.roughnessFactor = 0.4f;
            m.anisotropyStrength = 0.8f;
            palette.push_back(m);
        }
        const uint32_t kBlobMats = static_cast<uint32_t>(palette.size()) - 1u; // all but the floor material

        // --- Streamed assets: the floor is enqueued up front; blobs stream in on SPACE -----------
        Vector<StreamAsset> assets(GLOBAL_ALLOC);
        assets.reserve(kBlobCount + 1u); // stable addresses for the whole run
        {
            StreamAsset& floor = assets.emplace_back();
            floor.material = matFloor;
            FImportedMesh plane = MakePlane(GLOBAL_ALLOC);
            EnqueueAsync(floor, plane);
        }
        const size_t kFloorAsset = 0; // assets[0] is the floor; assets[1..] are blobs

        RendererUBO ubo{.adaptiveThreshold = 0.10f};

        ubo.ptSamplesPerPixel = 1;

        // Phyllotaxis placement: blob `i` sits on a golden-angle spiral so the field fills outward
        // from the centre as assets arrive. `t` adds a slow per-asset spin.
        auto BlobTransform = [&](uint32_t i, float t, float3& pos, quat& rot, float3& sca)
        {
            float a = static_cast<float>(i) * 2.39996323f;
            float rad = 0.34f * std::sqrt(static_cast<float>(i) + 0.5f);
            pos = float3(rad * std::cos(a), 0.34f, rad * std::sin(a));
            rot = angleAxis(t * 0.4f + static_cast<float>(i), float3(0, 1, 0));
            sca = float3(0.55f);
        };

        // Re-authors the scene each frame for the assets enqueued so far. Instances may reference
        // geometry that isn't resident yet - BuildTLAS simply skips those until Query reports Ready.
        auto AuthorFrame = [&](float t)
        {
            uint32_t blobs = static_cast<uint32_t>(assets.size()) - 1u; // minus the floor
            uint32_t instanceCount = 1u + blobs;
            auto tables = gpu.BeginScene(instanceCount, static_cast<uint32_t>(palette.size()), 2u);

            tables.instances[0] = InstanceDesc{.geometry = assets[kFloorAsset].handle,
                                               .transform = float3(0.0f),
                                               .rotation = angleAxis(0.0f, float3(0, 1, 0)),
                                               .scale = float3(16.0f, 1.0f, 16.0f),
                                               .materialIndex = matFloor};
            for (uint32_t i = 0; i < blobs; ++i)
            {
                float3 pos;
                quat rot;
                float3 sca;
                BlobTransform(i, t, pos, rot, sca);
                tables.instances[1 + i] = InstanceDesc{.geometry = assets[1 + i].handle,
                                                       .transform = pos,
                                                       .rotation = rot,
                                                       .scale = sca,
                                                       .materialIndex = assets[1 + i].material};
            }
            for (size_t i = 0; i < palette.size(); ++i)
                tables.materials[i] = palette[i];

            GSLight& env = tables.lights[0];
            env = GSLight{};
            env.flags = 5u; // Environment light, always present as the first light.
            env.color = float3(0.02f, 0.025f, 0.035f);
            env.power = 1.0f;
            env.importance = 0.035f;

            GSLight& key = tables.lights[1];
            key = GSLight{};
            key.flags = 4u | kGSLightFlagUseShadow; // Rect area light
            key.color = float3(1.0f, 0.96f, 0.9f);
            key.power = 12.0f;
            key.position = float3(0.0f, 6.0f, 0.0f);
            key.dpdu = float3(2.2f, 0.0f, 0.0f);
            key.dpdv = float3(0.0f, 0.0f, 2.2f);
            key.direction = float3(0.0f, -1.0f, 0.0f);
            key.importance = key.power;

            gpu.EndScene(tables, ubo.frameNumber);
            gpu.BuildUBO(ubo);
        };
        AuthorFrame(0.0f); // seed the tables (floor only) so the initial TLAS build is well-formed

        // One-time TLAS build sizes the AS inside its pre-allocated 16 MB buffer (the only sync, and
        // it's before the loop). From here the graph's per-frame TLAS Update keeps it current.
        {
            ImmediateContext ctx(RHIDeviceQueueType::Compute, device.Get());
            auto* cmd = ctx.Get();
            cmd->Begin();
            auto tlasResult = gpu.BuildTLAS(cmd, /*update*/ false);
            cmd->End();
            if (tlasResult == GPUScene::TLASBuildResult::Built)
                ctx.Submit(), ctx.WaitIdle();
        }

        ExampleGPUSceneRenderState renderState{};

        // On-screen HUD: the CSDebugText pass draws on top of the scene's backbuffer, reading from
        // input.hud (persistent, re-read every frame) via Examples_GPUSceneBuildRenderGraph.
        ExampleInputState input{};

        auto RecreateRenderer = [&]
        { renderer = ConstructUnique<Renderer>(GLOBAL_ALLOC, rendererDesc, device, swapchain, GLOBAL_ALLOC); };
        auto BuildGraph = [&](RHIExtent2D extent)
        { Examples_GPUSceneBuildRenderGraph(renderer.get(), &ubo, &gpu, renderState, input, extent); };
        BuildGraph(renderer->GetSwapchainExtent());

        // Example orbit camera: mouse/touch drag orbits, WASD dollies.
        FExampleOrbitCamera camera{.center = {0.0f, 0.5f, 0.0f},
                                   .radius = 6.0f,
                                   .rot = normalize(angleAxis(radians(-18.0f), float3(1, 0, 0))),
                                   .zNear = 0.01f,
                                   .fovY = radians(50.0f)};

        // Streaming is manual: each SPACE press enqueues a wave of procedural meshes. Generating +
        // uploading them returns immediately (no Join), so the press never stalls - the meshes stream
        // into the live scene as the background worker builds their BLASes.
        auto SpawnWave = [&]
        {
            uint32_t spawned = 0u;
            while (spawned < kSpawnPerPress && assets.size() < static_cast<size_t>(kBlobCount + 1u))
            {
                uint32_t blobIndex = static_cast<uint32_t>(assets.size()) - 1u; // assets[0] is the floor
                StreamAsset& a = assets.emplace_back();
                a.material = 1u + (blobIndex % kBlobMats); // skip the floor material
                FImportedMesh blob = MakeBlob(GLOBAL_ALLOC, blobIndex + 1u);
                EnqueueAsync(a, blob);
                ++spawned;
            }
            return spawned > 0u;
        };

        // Releases every streamed blob and reclaims its GPU memory - the same release path the editor
        // takes on a destructive edit: drop the instances, then GPUScene::Collect() frees the geometry
        // no longer referenced by the committed table. (Collect, not Reset: we keep the scene + its
        // pre-allocated buffers; only the unreferenced resources are recycled, ready to re-stream.)
        auto ReleaseBlobs = [&]
        {
            gpu.Join(); // upload worker is done touching the blob payloads we're about to free
            assets.resize(1); // keep the floor (assets[0]); drop the blob payloads + their handles
            AuthorFrame(0.0f); // re-commit floor-only so the blob geometry is now unreferenced
            device->WaitIdle(); // let in-flight frames that still reference the blob BLASes finish
            gpu.Collect(); // reclaim the unreferenced geometry (slots + primitive memory)
            ubo.ptAccumulatedFrames = 0u;
        };

        ExampleFpsCounter fps;
        uint64_t lastTicks = SDL_GetTicksNS();
        bool paused = false; // P: freeze the scene (blobs + accumulation) so the path tracer converges
        float animTime = 0.0f;
        while (true)
        {
            Examples_BeginFrameInput(input);
            if (Examples_PollEvents(window, renderer.get(), swapchain, input))
                break;

            uint64_t now = SDL_GetTicksNS();
            float dt = static_cast<float>(now - lastTicks) / 1e9f;
            lastTicks = now;

            RHIExtent2D currentExtent = renderer->GetSwapchainExtent();
            if (currentExtent.x == 0u || currentExtent.y == 0u)
                continue;
            if (currentExtent.x != renderState.renderExtent.x || currentExtent.y != renderState.renderExtent.y)
            {
                device->WaitIdle();
                RecreateRenderer();
                BuildGraph(currentExtent);
            }

            // Producer side: SPACE enqueues the next wave from inside the render loop. The call
            // returns immediately - the worker drains it concurrently while we keep rendering.
            bool spawnedThisFrame = false;
            bool atCap = assets.size() >= static_cast<size_t>(kBlobCount + 1u);
            Examples_BeginControls(input);
            Examples_Text(input, "GPUScene async streaming");
            const bool streamPressed = Examples_Button(input,
                atCap ? "[Release]" : fmt::format("[Stream +{}]", kSpawnPerPress)) ||
                input.KeyPressed(SDLK_SPACE);
            Examples_SameLine(input);
            const bool toggleRenderer = Examples_Button(input,
                fmt::format("[{}]", Examples_GPUSceneModeName(renderState.mode))) ||
                input.KeyPressed(SDLK_TAB);
            Examples_SameLine(input);
            const bool togglePause = Examples_Button(input, paused ? "[Resume]" : "[Pause]") ||
                input.KeyPressed(SDLK_F);
            const bool resolutionChanged =
                Examples_Slider(input, "Resolution", renderState.renderScale, 0.10f, 1.0f, 0.05f, "x", false);
            Examples_Text(input, FExampleOrbitCamera::kControlsText);
            if (streamPressed)
            {
                // Stream the next wave, or - once the field is full - release everything and loop.
                if (assets.size() >= static_cast<size_t>(kBlobCount + 1u))
                    ReleaseBlobs();
                else
                    spawnedThisFrame = SpawnWave();
            }
            if (togglePause)
                paused = !paused;
            if (toggleRenderer || resolutionChanged)
            {
                if (toggleRenderer)
                    Examples_GPUSceneToggleMode(renderState);
                device->WaitIdle();
                RecreateRenderer();
                BuildGraph(renderState.renderExtent);
                ubo.ptAccumulatedFrames = 0u;
            }

            // Observe residency (per-resource, lock-free from the caller's view) for the readout.
            GPUScene::Result drain = gpu.Poll(); // also starts the background worker on first call
            uint32_t resident = 0;
            for (StreamAsset const& a : assets)
                resident += gpu.Query(a.handle) == GPUScene::Result::Ready ? 1u : 0u;
            bool streaming = drain == GPUScene::Result::InProgress;

            if (!paused)
                animTime += dt; // blobs spin while not paused
            camera.aspect = static_cast<float>(renderState.renderExtent.x) / static_cast<float>(renderState.renderExtent.y);
            bool cameraMoved = camera.Update(input, dt);

            ubo.frameNumber = renderer->GetFrame();
            AuthorFrame(animTime);

            Examples_GPUSceneFillCameraUBO(ubo, renderer.get(), camera, renderState.config);

            // Refresh the HUD readout before submitting (the overlay pass reads it this frame).
            atCap = assets.size() >= static_cast<size_t>(kBlobCount + 1u);
            Examples_Text(input,
                          atCap ? fmt::format("resident {} / {}  -  full: stream releases + loops   {:.0f} FPS{}",
                                              resident, static_cast<uint32_t>(assets.size()), fps.Update(),
                                              paused ? "   [PAUSED]" : "")
                                : fmt::format("resident {} / {} (max {})   {:.0f} FPS{}", resident,
                                              static_cast<uint32_t>(assets.size()), kBlobCount + 1u, fps.Update(),
                                              paused          ? "   [PAUSED]"
                                                  : streaming ? "   [streaming...]"
                                                              : ""));

            Examples_NewFrame(window, renderer.get(), swapchain);
            if (renderState.mode == ExampleGPUSceneRenderMode::PathTracer)
                ubo.ptAccumulatedFrames += ubo.ptSamplesPerPixel;
            // The scene changes whenever it streams, a wave spawns, the camera moves, or the blobs
            // spin (not paused), so restart accumulation; pause + hold still to let it converge.
            if (streaming || spawnedThisFrame || cameraMoved || !paused)
                ubo.ptAccumulatedFrames = 0;
        }

        device->WaitIdle();
    }
    Examples_DestroyVulkan(window, renderer.release(), app, device, swapchain);
    return 0;
}
