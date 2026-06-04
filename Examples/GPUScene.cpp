// Example: drive GPUScene + the Foundation renderer[s] end-to-end as a light-transport demo.
//
// This is a "material gallery in a Cornell box": the classic setup for showing off a path
// tracer and a principled BSDF, because every interesting transport effect is visible at once.
//   - A 5-sided box (white floor/ceiling/back, red left wall, green right wall) so indirect
//     bounces tint everything -> diffuse global illumination / colour bleeding.
//   - A single soft rectangular ceiling light -> physically-soft penumbrae instead of the hard
//     point-light shadows the old "bunny galaxy" used.
//   - A row of bunnies, each wearing one principled material that leans on a different part of
//     the BSDF: smooth glass (transmission + IOR -> refraction & caustics), polished gold
//     (perfect specular -> mirror reflections that pick up the coloured walls), waxy subsurface
//     (BSSRDF -> light bleeding through thin features), brushed metal (anisotropy -> stretched
//     highlights) and clearcoat car-paint (dual specular lobe).
//
// Mechanics are the same flow the editor uses, minus the UI:
//   1. Load + process the bunny, and synthesize a tessellated plane for the box walls.
//   2. Serialize each into an in-memory blob payload and Upload() it to GPUScene.
//   3. BeginScene/EndScene each frame to fill the instance/material/light tables.
//   4. FillGlobals + BuildTLAS so the render graph has everything it needs (area lights are
//      injected into the TLAS automatically by GPUScene).
//   5. BuildPathTracer/RasterRenderGraph once; it presents straight to the backbuffer.
//   6. Per frame: drive the UBO from an FArcballCamera (WASD + mouse) and submit.
#include <Renderer/GPUScene.hpp>
#include <Renderer/Renderer.hpp> // UBO + BuildRasterRenderGraph + Renderer{Scene,Config,Handles}
#include <Editor/Scene/Mesh.hpp> // FImportedMesh / LoadObj
#include <Editor/Camera.hpp>     // FArcballCamera
#include <RenderUtils/CSDebugText.hpp> // createCSDebugTextPassBackBuffer (on-screen HUD)
#include <Core/Paths.hpp>
#include "Examples.hpp"
#include <algorithm>
#include <cmath>

using namespace RenderUtils;
using Foundation::Core::PathsResolve;

int main(int argc, char** argv)
{
    SDL_Window* window = SDL_CreateWindow("Material Gallery", 1280, 720, Examples_SDLWindowFlagsVulkan);
    RendererDesc rendererDesc{};
    auto [renderer0, app, device, swapchain] = Examples_InitVulkan(window, argc, argv, rendererDesc);
    // Own the renderer's lifetime: a resize tears it down (dtor waits for GPU idle) and
    // rebuilds it against the resized swapchain, which is cleaner than re-running setup on a
    // live renderer. Adopt the instance Examples_InitVulkan created.
    UniquePtr<Renderer> renderer(renderer0, StlDeleter<Renderer>{GLOBAL_ALLOC});

    // GPUScene owns all GPU-resident scene data and lives for the full example scope.
    GPUScene gpu(device.Get(), GLOBAL_ALLOC, GPUSceneDesc{});

    // Runs a CPU mesh through the same pipeline the editor uses (optimize -> DAG clusterize ->
    // quantize), serializes it into a throwaway blob payload, and uploads it synchronously.
    // The payload only has to outlive the Upload + Join, both of which happen here.
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

    // --- 1+2. Load the bunny + build a plane for the walls, then upload both ----------------
    // Bunny AABB (raw, pre-quantize) so we can scale every copy to a fixed height and stand it
    // on the floor regardless of the source model's units/pivot.
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
    // A unit plane in the XZ plane (spanning [-0.5,0.5], +Y normal), lightly tessellated so the
    // clusterizer has something to work with. Instanced + rotated into the five box walls.
    GeometryHandle plane;
    {
        constexpr int seg = 16;
        FImportedMesh mesh(GLOBAL_ALLOC);
        auto& lod = mesh.lods[0];
        for (int z = 0; z <= seg; ++z)
            for (int x = 0; x <= seg; ++x)
            {
                FVertex v{};
                v.position = float3(static_cast<float>(x) / seg - 0.5f, 0.0f, static_cast<float>(z) / seg - 0.5f);
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
                lod.indices.push_back(i0); lod.indices.push_back(i2); lod.indices.push_back(i1);
                lod.indices.push_back(i1); lod.indices.push_back(i2); lod.indices.push_back(i3);
            }
        plane = UploadMesh(mesh);
    }

    // --- 3. Author the scene (re-filled every frame so the bunnies turn) ---------------------
    constexpr float kPi = 3.14159265358979f;
    constexpr float boxW = 4.0f; // interior width  (x in [-2, 2])
    constexpr float boxH = 3.0f; // interior height (y in [ 0, 3])
    constexpr float boxD = 4.0f; // interior depth  (z in [-2, 2])
    UBO ubo{};

    // -- Material palette. All principled (shaderBlockID 0); each hero leans on a different
    //    part of the BSDF so the same path tracer renders glass, metal, wax and lacquer.
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
    // Walls: matte Lambertian-ish surfaces; the red/green pair is what bleeds colour everywhere.
    const uint32_t matWhite = static_cast<uint32_t>(palette.size());
    { GSMaterial m = BaseMat(); m.baseColorFactor = float4(0.725f, 0.71f, 0.68f, 1.0f); m.roughnessFactor = 1.0f; palette.push_back(m); }
    const uint32_t matRed = static_cast<uint32_t>(palette.size());
    { GSMaterial m = BaseMat(); m.baseColorFactor = float4(0.63f, 0.065f, 0.05f, 1.0f); m.roughnessFactor = 1.0f; palette.push_back(m); }
    const uint32_t matGreen = static_cast<uint32_t>(palette.size());
    { GSMaterial m = BaseMat(); m.baseColorFactor = float4(0.14f, 0.45f, 0.091f, 1.0f); m.roughnessFactor = 1.0f; palette.push_back(m); }
    // Heroes.
    const uint32_t matGlass = static_cast<uint32_t>(palette.size());
    { GSMaterial m = BaseMat(); m.baseColorFactor = float4(0.92f, 0.96f, 1.0f, 1.0f); m.roughnessFactor = 0.02f; m.transmissionFactor = 1.0f; m.ior = 1.5f; palette.push_back(m); }
    const uint32_t matGold = static_cast<uint32_t>(palette.size());
    { GSMaterial m = BaseMat(); m.baseColorFactor = float4(1.0f, 0.84f, 0.40f, 1.0f); m.metallicFactor = 1.0f; m.roughnessFactor = 0.05f; palette.push_back(m); }
    const uint32_t matWax = static_cast<uint32_t>(palette.size());
    {
        GSMaterial m = BaseMat();
        m.baseColorFactor = float4(1.0f, 0.82f, 0.66f, 1.0f);
        m.roughnessFactor = 0.45f;
        m.subsurfaceFactor = 1.0f;
        m.subsurfaceColor = float3(0.9f, 0.35f, 0.25f);
        m.subsurfaceRadius = float3(1.0f, 0.45f, 0.30f); // per-channel mean free path (R bleeds farthest)
        m.subsurfaceScale = 0.18f;                       // scales radius into world units
        palette.push_back(m);
    }
    const uint32_t matBrushed = static_cast<uint32_t>(palette.size());
    {
        GSMaterial m = BaseMat();
        m.baseColorFactor = float4(0.92f, 0.93f, 0.96f, 1.0f);
        m.metallicFactor = 1.0f;
        m.roughnessFactor = 0.35f;
        m.anisotropyStrength = 0.85f; // stretches the GGX highlight tangentially
        palette.push_back(m);
    }
    const uint32_t matPaint = static_cast<uint32_t>(palette.size());
    {
        GSMaterial m = BaseMat();
        m.baseColorFactor = float4(0.03f, 0.06f, 0.45f, 1.0f);
        m.roughnessFactor = 0.5f;
        m.clearcoatFactor = 1.0f;            // glossy dielectric lobe over the coloured base
        m.clearcoatRoughnessFactor = 0.04f;
        palette.push_back(m);
    }

    // -- A grid of bunnies: one material per column (each BSDF gets its own lane) repeated over a
    //    few rows. Scaled to a fixed height and spaced so their footprints don't collide.
    const uint32_t colMats[] = {matGlass, matGold, matWax, matBrushed, matPaint};
    constexpr int kCols = static_cast<int>(sizeof(colMats) / sizeof(colMats[0]));
    constexpr int kRows = 3;
    const float3 bunnyCenter = (bunnyMin + bunnyMax) * 0.5f;
    const float bunnyScale = 0.55f / std::max(bunnyMax.y - bunnyMin.y, 1e-4f);
    const float footX = (bunnyMax.x - bunnyMin.x) * bunnyScale;
    const float footZ = (bunnyMax.z - bunnyMin.z) * bunnyScale;
    const float spacing = std::min(0.74f, std::max(0.60f, std::sqrt(footX * footX + footZ * footZ) * 0.85f));

    // Jump cadence: each bunny hops for half its cycle and rests (crouched) the other half, with a
    // position-based phase offset so the hop travels across the grid as a wave.
    constexpr float kJumpFreq = 4.2f;    // rad/s
    constexpr float kJumpHeight = 0.45f; // world units at the apex
    constexpr float kStretchAmp = 0.35f; // vertical stretch while airborne
    constexpr float kSquashAmp = 0.30f;  // vertical squash while crouched

    // Four walls (floor/back/left/right, open top) + the bunny grid. Constant count, so the
    // per-frame TLAS refit never needs a full rebuild.
    const uint32_t instanceCount = 4u + static_cast<uint32_t>(kCols * kRows);
    const uint32_t lightCount = 1u;

    // Re-authors the whole scene for animation time `t`: static box, the jumping/squashing bunny
    // grid, and the overhead area light, then writes the ring-buffer offsets into the UBO. Called once up
    // front (so the initial TLAS build has instances) and then every frame from the main loop.
    auto AuthorFrame = [&](float t)
    {
        auto tables = gpu.BeginScene(instanceCount, static_cast<uint32_t>(palette.size()), lightCount);

        uint32_t idx = 0u;
        auto AddWall = [&](quat rot, float3 scale, float3 pos, uint32_t mat)
        {
            tables.instances[idx++] = InstanceDesc{.geometry = plane, .transform = pos, .rotation = rot, .scale = scale, .materialIndex = mat};
        };
        // Each wall is the unit plane rotated so its +Y normal points into the room. The
        // instance transform scales in *world* axes after rotation (world = Scale * Rot * local),
        // so the scale vector gives each wall's world-space extents directly. Open top, no
        // ceiling: the box stays well-lit in the raster path and the area light reads cleanly.
        AddWall(angleAxis(0.0f, float3(1, 0, 0)),        float3(boxW, 1.0f, boxD), float3(0.0f, 0.0f, 0.0f),                matWhite); // floor
        AddWall(angleAxis(kPi * 0.5f, float3(1, 0, 0)),  float3(boxW, boxH, 1.0f), float3(0.0f, boxH * 0.5f, -boxD * 0.5f), matWhite); // back
        AddWall(angleAxis(-kPi * 0.5f, float3(0, 0, 1)), float3(1.0f, boxH, boxD), float3(-boxW * 0.5f, boxH * 0.5f, 0.0f), matRed);   // left
        AddWall(angleAxis(kPi * 0.5f, float3(0, 0, 1)),  float3(1.0f, boxH, boxD), float3(boxW * 0.5f, boxH * 0.5f, 0.0f),  matGreen); // right

        for (int row = 0; row < kRows; ++row)
            for (int col = 0; col < kCols; ++col)
            {
                int n = row * kCols + col;
                float px = (static_cast<float>(col) - (kCols - 1) * 0.5f) * spacing;
                float pz = (static_cast<float>(row) - (kRows - 1) * 0.5f) * spacing - 0.10f;

                // Hop: airborne for half the cycle (sin > 0), crouched on the floor for the rest.
                float phase = t * kJumpFreq + (static_cast<float>(col) * 0.7f + static_cast<float>(row) * 1.3f);
                float s = std::sin(phase);
                float yJump = kJumpHeight * std::max(0.0f, s);
                // Volume-preserving squash & stretch: tall & thin through the fast parts of the
                // arc, flat & wide while crouched; sxz = 1/sqrt(sy) holds the volume constant.
                float sy = s > 0.0f ? 1.0f + kStretchAmp * std::abs(std::cos(phase))
                                    : 1.0f - kSquashAmp * (-s);
                float sxz = 1.0f / std::sqrt(sy);
                float3 sca = float3(bunnyScale * sxz, bunnyScale * sy, bunnyScale * sxz);

                // Gentle turntable so reflections/highlights/caustics drift across the materials.
                float yaw = t * 0.25f + static_cast<float>(n) * 0.5f;
                quat rot = angleAxis(yaw, float3(0, 1, 0));
                // Compensate the model pivot (scale is world-space, applied after rotation) so each
                // bunny spins about its own centre and keeps its feet planted as it squashes/lifts.
                float3 v = rot * float3(bunnyCenter.x * bunnyScale, 0.0f, bunnyCenter.z * bunnyScale);
                tables.instances[idx++] = InstanceDesc{.geometry = bunny,
                                                       .transform = float3(px - sxz * v.x,
                                                                           yJump - bunnyMin.y * bunnyScale * sy,
                                                                           pz - sxz * v.z),
                                                       .rotation = rot,
                                                       .scale = sca,
                                                       .materialIndex = colMats[col]};
            }

        for (size_t i = 0; i < palette.size(); ++i)
            tables.materials[i] = palette[i];

        // Single soft rectangular overhead light. `power` is emitted radiance (Lemit = colour *
        // power); dpdu/dpdv are half-extent vectors so this is a 1.4 x 1.4 quad facing straight
        // down. GPUScene adds it to the TLAS as area-light geometry; the path tracer samples it
        // with MIS for soft shadows + GI, the raster path treats it as a point at its centre.
        GSLight& key = tables.lights[0];
        key = GSLight{};
        key.type = 4u; // Rect
        key.color = float3(1.0f, 0.95f, 0.88f);
        key.power = 10.0f;
        key.position = float3(0.0f, boxH - 0.02f, 0.0f);
        key.dpdu = float3(0.70f, 0.0f, 0.0f);
        key.dpdv = float3(0.0f, 0.0f, 0.70f);
        key.direction = float3(0.0f, -1.0f, 0.0f);
        key.twoSided = 0u;
        key.selectionWeight = key.power; // only one light, so any positive weight works

        auto result = gpu.EndScene(tables);
        ubo.firstInstance = result.firstInstance;
        ubo.numInstances = result.numInstances;
        ubo.firstMaterial = result.firstMaterial;
        ubo.numMaterials = result.numMaterials;
        ubo.firstLight = result.firstLight;
        ubo.firstLightAliasTable = result.firstLightAliasTable;
        ubo.numSceneLights = result.numLights;
        ubo.sceneLightWeightSum = result.sceneLightWeightSum;
    };
    AuthorFrame(0.0f); // seed the tables so the initial TLAS build has instances
    ubo.ptDispatchTileSide = 1;
    ubo.ptSamplesPerPixel = 1;

    // --- 4. Globals + TLAS ---------------------------------------------------------------
    gpu.BuildUBO(ubo);
    ubo.ambientColor = float3(0.0f); // pitch-black surround: all light comes from the ceiling quad
    ubo.ambientPower = 1.0f;
    ubo.useEnvMap = 0u;
    TextureHandle viewLUTSdrHandle{};
    TextureHandle viewLUTHdrHandle{};
    {
        ImmediateContext ctx(RHIDeviceQueueType::Compute, device.Get());
        auto* cmd = ctx.Get();
        cmd->Begin();
        auto tlasResult = gpu.BuildTLAS(cmd, /*update*/ false);
        cmd->End();
        if (tlasResult == GPUScene::TLASBuildResult::Built)
            ctx.Submit(), ctx.WaitIdle();
    }

    // --- 5. Build the render graph (presents to the backbuffer itself) -------------------
    // TAB toggles between the two renderers GPUScene feeds: meshlet raster and path tracer.
    enum class Mode { Raster, PathTracer };
    Mode mode = Mode::PathTracer;
    bool renderPaused = false; // path tracer accumulation gate (never paused here)
    RendererConfig cfg{};
    PostprocessUBO postprocessGlobals{};
    RendererOutputs handles{};
    RHIExtent2D renderExtent{};
    // On-screen HUD: the CSDebugText pass draws over the scene's backbuffer. The array is
    // persistent and the pass captures a view of it, re-reading the current text every frame.
    CSDebugTextData hud[3]{};
    hud[0].x = 16; hud[0].y = 16; hud[0].SetText("Material Gallery (Cornell Box)");
    hud[1].x = 16; hud[1].y = 40; hud[1].SetText("TAB renderer   SPACE pause/converge   WASD + drag camera");
    hud[2].x = 16; hud[2].y = 64;
    // Builds the active mode's graph on the current (fresh, setup-ready) renderer for `extent`.
    auto BuildGraph = [&](RHIExtent2D extent)
    {
        cfg.renderExtent = extent;
        cfg.ptRenderPaused = &renderPaused;
        renderer->BeginSetup();
        if (mode == Mode::PathTracer)
            BuildPathTracerRenderGraph(renderer.get(), &ubo, &gpu, cfg, handles);
        else
            BuildRasterRenderGraph(renderer.get(), &ubo, &gpu, cfg, handles);
        Examples_InsertBasicTonemapPasses(
            renderer.get(), gpu, handles, cfg, &postprocessGlobals, viewLUTSdrHandle, viewLUTHdrHandle);
        createCSDebugTextPassBackBuffer(renderer.get(), "Debug Text", hud); // overlay last
        renderer->EndSetup();
        renderExtent = extent;
    };
    // The graph sizes its internal targets to the render extent, so on a resize we tear the
    // renderer down and recreate it against the resized swapchain before rebuilding the graph.
    auto RecreateRenderer = [&]
    {
        renderer = ConstructUnique<Renderer>(GLOBAL_ALLOC, rendererDesc, device, swapchain, GLOBAL_ALLOC);
    };
    BuildGraph(renderer->GetSwapchainExtent());

    // --- 6. Main loop --------------------------------------------------------------------
    // Look into the open front of the box from slightly above the bunnies.
    FArcballCamera camera{.center = {0.0f, 0.95f, 0.0f},
                          .radius = 5.2f,
                          .rot = normalize(angleAxis(radians(-4.0f), float3(1, 0, 0))),
                          .zNear = 0.01f,
                          .fovY = radians(45.0f)};
    ExampleFpsCounter fps;
    SDL_Event event{};
    uint64_t lastTicks = SDL_GetTicksNS();
    bool cameraPaused = false; // SPACE: freeze camera + animation so the path tracer converges
    float animTime = 0.0f;     // scene clock; only advances while not paused
    while (!Examples_ShouldClose(window, renderer.get(), swapchain, &event))
    {
        uint64_t now = SDL_GetTicksNS();
        float dt = static_cast<float>(now - lastTicks) / 1e9f;
        lastTicks = now;

        // Examples_ShouldClose recreates the swapchain on resize; recreate the renderer and
        // rebuild the graph to match the new extent. Skip the frame while minimized (0 extent).
        RHIExtent2D currentExtent = renderer->GetSwapchainExtent();
        if (currentExtent.x == 0u || currentExtent.y == 0u)
            continue;
        if (currentExtent.x != renderExtent.x || currentExtent.y != renderExtent.y)
        {
            device->WaitIdle();
            RecreateRenderer();
            BuildGraph(currentExtent);
        }

        camera.aspect = static_cast<float>(renderExtent.x) / static_cast<float>(renderExtent.y);
        // WASD fly-through is tracked here; the editor mirrors this key handling.
        if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP)
        {
            bool pressed = event.type == SDL_EVENT_KEY_DOWN;
            switch (event.key.key)
            {
            case SDLK_W: camera.keyW = pressed; break;
            case SDLK_A: camera.keyA = pressed; break;
            case SDLK_S: camera.keyS = pressed; break;
            case SDLK_D: camera.keyD = pressed; break;
            case SDLK_LSHIFT: case SDLK_RSHIFT: camera.keyShift = pressed; break;
            case SDLK_SPACE: cameraPaused ^= pressed; break;
            default: break;
            }
        }
        // TAB switches renderer: rebuild the renderer + graph for the new mode.
        if (event.type == SDL_EVENT_KEY_DOWN && event.key.key == SDLK_TAB && !event.key.repeat)
        {
            mode = mode == Mode::Raster ? Mode::PathTracer : Mode::Raster;
            device->WaitIdle();
            RecreateRenderer();
            BuildGraph(renderExtent);
            ubo.ptAccumulatedFrames = 0u;
        }
        // The bunnies turn slowly; frozen (with the scene clock) while paused so the path tracer
        // can accumulate a still, converged image.
        if (!cameraPaused)
            animTime += dt;
        bool cameraMoved = camera.UpdateMovement(dt);
        cameraMoved |= camera.Update(event);

        // Re-author the animated scene for this frame. The graph's TLAS-update pass refits
        // against these committed instances, so this is all the per-frame motion needs.
        AuthorFrame(animTime);

        ubo.frameNumber = renderer->GetFrame();
        ubo.view = camera.view;
        ubo.proj = camera.proj;
        ubo.inverseView = inverse(camera.view);
        ubo.inverseViewProj = inverse(camera.proj * camera.view);
        ubo.zNear = camera.zNear;
        ubo.projPlanes = planeSymmetric(camera.proj);
        ubo.camPosition = float4(camera.position, 0.0f);
        ubo.camDirection = float4(camera.rot * float3(0, 0, -1), 0.0f);
        ubo.fbWidth = static_cast<float>(renderExtent.x);
        ubo.fbHeight = static_cast<float>(renderExtent.y);
        ubo.ptViewFlags = cfg.viewFlags;
        postprocessGlobals.camEV = ubo.camEV;
        postprocessGlobals.postShowOutline = 0u;
        postprocessGlobals.ptAccumulatedFrames = ubo.ptAccumulatedFrames;
        postprocessGlobals.ptDispatchTileSide = ubo.ptDispatchTileSide;
        postprocessGlobals.fbWidth = ubo.fbWidth;
        postprocessGlobals.fbHeight = ubo.fbHeight;

        // Refresh the HUD readout before submitting (the overlay pass reads it this frame).
        hud[2].SetText(fmt::format("{}   {:.0f} FPS{}", mode == Mode::PathTracer ? "Path Tracer" : "Raster",
                                   fps.Update(), cameraPaused ? "   [PAUSED]" : ""));

        Examples_NewFrame(renderer.get());
        // Advance path-tracer accumulation after the frame is submitted.
        if (mode == Mode::PathTracer)
            ubo.ptAccumulatedFrames += PTSamplesPerDispatch(ubo);
        if (!cameraPaused || cameraMoved)
            ubo.ptAccumulatedFrames = 0;
    }

    device->WaitIdle();
    // Hand the renderer back as a raw pointer; Examples_DestroyVulkan owns the teardown.
    Examples_DestroyVulkan(window, renderer.release(), app, device, swapchain);
    return 0;
}
