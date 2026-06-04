// Example: CPU-updateable dynamic geometry in GPUScene (BLAS refit / rebuild).
//
// A tessellated grid is uploaded once as *dynamic* geometry (GPUScene::UploadDynamic). Its
// topology is fixed; every frame the host rewrites only the quantized vertex positions of the
// current ring slot (a sine "ripple" deformation), marks it dirty, and the render graph's
// "Dynamic BLAS Refit" pass refits the single AllowUpdate BLAS in place before the TLAS update.
// A static floor + the immutable streaming path are left untouched, demonstrating that dynamic
// geo is purely additive.
//
// What to look for:
//   - The grid ripples every frame in both renderers. In the path tracer the deformed surface is
//     traced (BLAS refit feeds the TLAS); in raster it is drawn through a dedicated indexed
//     multi-draw (no meshlets/DAG) and, with RT shadows on, also refits for the shadow trace.
//   - The HUD shows refit vs. rebuild counts. R toggles between refit-only and a periodic
//     rebuild cadence; B forces a full rebuild every frame (the quality-vs-cost trade-off).
//
// Controls: TAB renderer; R refit-only/periodic; B rebuild-every-frame; P pause; WASD + drag camera.
#include <Renderer/GPUScene.hpp>
#include <Renderer/Renderer.hpp>
#include <Renderer/Mesh.hpp>      // FVertex / FQVertex
#include <Editor/Scene/Mesh.hpp>  // FImportedMesh / FSerializedMesh / MemoryBlobSerializer
#include <Editor/Camera.hpp>      // FArcballCamera
#include <RenderUtils/CSDebugText.hpp>
#include <Core/Paths.hpp>
#include "Examples.hpp"
#include <cmath>

using namespace RenderUtils;
using Foundation::Core::PathsResolve;

namespace
{
constexpr float kPi = 3.14159265358979f;
constexpr int kGridSeg = 96;        // grid resolution (kGridSeg+1 squared vertices)
constexpr float kGridSize = 3.0f;   // world extent of the grid (XZ)
constexpr float kRippleAmp = 0.18f; // deformation amplitude (world units)
constexpr float kRippleFreq = 3.2f; // spatial frequency

// One grid vertex's static attributes (rest pose lives implicitly in (x,z); y is deformed).
struct GridVertex
{
    float x, z; // planar position in [-0.5,0.5] * kGridSize
    float u, v;
};

// A unit XZ plane (+Y), lightly tessellated, used as the static floor (immutable path).
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

GSMaterial BaseMat()
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
}
} // namespace

int main(int argc, char** argv)
{
    SDL_Window* window = SDL_CreateWindow("GPUScene Dynamic Geometry (BLAS refit)", 1280, 720,
                                          Examples_SDLWindowFlagsVulkan);
    RendererDesc rendererDesc{};
    auto [renderer0, app, device, swapchain] = Examples_InitVulkan(window, argc, argv, rendererDesc);
    UniquePtr<Renderer> renderer(renderer0, StlDeleter<Renderer>{GLOBAL_ALLOC});

    // --- Build the deforming grid's fixed topology + static (x,z,uv) attributes -------------
    Vector<GridVertex> gridVertices(GLOBAL_ALLOC);
    Vector<uint32_t> gridIndices(GLOBAL_ALLOC);
    for (int z = 0; z <= kGridSeg; ++z)
        for (int x = 0; x <= kGridSeg; ++x)
        {
            float fx = static_cast<float>(x) / kGridSeg - 0.5f;
            float fz = static_cast<float>(z) / kGridSeg - 0.5f;
            gridVertices.push_back({fx * kGridSize, fz * kGridSize,
                            static_cast<float>(x) / kGridSeg, static_cast<float>(z) / kGridSeg});
        }
    auto VID = [&](int r, int s) { return static_cast<uint32_t>(r * (kGridSeg + 1) + s); };
    for (int z = 0; z < kGridSeg; ++z)
        for (int x = 0; x < kGridSeg; ++x)
        {
            uint32_t i0 = VID(z, x), i1 = VID(z, x + 1), i2 = VID(z + 1, x), i3 = VID(z + 1, x + 1);
            gridIndices.push_back(i0); gridIndices.push_back(i2); gridIndices.push_back(i1);
            gridIndices.push_back(i1); gridIndices.push_back(i2); gridIndices.push_back(i3);
        }
    const uint32_t gridVtxCount = static_cast<uint32_t>(gridVertices.size());
    const uint32_t gridIdxCount = static_cast<uint32_t>(gridIndices.size());

    // Quantized vertex evaluation for animation time `t`. Writes the rest pose at t<0.
    auto EvalGridVertex = [&](GridVertex const& g, float t) -> FQVertex
    {
        float phaseX = kRippleFreq * g.x + t;
        float phaseZ = kRippleFreq * g.z + t * 0.7f;
        float y = kRippleAmp * std::sin(phaseX) * std::sin(phaseZ);
        // Analytic normal of y = A sin(fx*x+t) sin(fz*z+t').
        float dydx = kRippleAmp * kRippleFreq * std::cos(phaseX) * std::sin(phaseZ);
        float dydz = kRippleAmp * kRippleFreq * std::sin(phaseX) * std::cos(phaseZ);
        FVertex v{};
        v.position = float3(g.x, y, g.z);
        v.normal = normalize(float3(-dydx, 1.0f, -dydz));
        v.tangent = normalize(float3(1.0f, dydx, 0.0f));
        v.bitangentSign = 1.0f;
        v.uv = float2(g.u, g.v);
        return FQVertex::Pack(v);
    };

    // GPUScene with dynamic geometry enabled. Budget covers the grid's per-slot footprint
    // (header + quantized verts + indices) with headroom; 3 ring slots (frames in flight).
    GPUSceneDesc desc{};
    desc.geometryBudget = 16u;
    desc.instanceBudget = 64u;
    desc.tlasInstanceBudget = 64u;
    desc.materialBudget = 16u;
    desc.lightBudget = 8u;
    desc.dynamicGeometryBudget = 16u * (1u << 20);          // 16 MiB / frame slot
    desc.framesInFlight = renderer->GetFrameSwaps();         // ring sized to frames-in-flight (+1 internally)
    {
        GPUScene gpu(device.Get(), GLOBAL_ALLOC, desc);

        // --- Upload the static floor (immutable path) -------------------------------------------
        GeometryHandle floor;
        Vector<unsigned char> floorPayload(GLOBAL_ALLOC);
        FSerializedMesh floorSerialized(GLOBAL_ALLOC);
        {
            FImportedMesh plane = MakePlane(GLOBAL_ALLOC);
            plane.Optimize();
            plane.ClusterizeDAG();
            CHECK_MSG(plane.EnsureQuantized(), "Failed to quantize floor");
            MemoryBlobSerializer blobs(floorPayload);
            floorSerialized.vertices = blobs.AppendArray(plane.verticesQuantized);
            floorSerialized.vertexCount = static_cast<uint32_t>(plane.verticesQuantized.size());
            FSerializedMeshLOD& lod0 = floorSerialized.lods.emplace_back();
            lod0.indices = blobs.AppendArray(plane.lods[0].indices);
            lod0.indexCount = static_cast<uint32_t>(plane.lods[0].indices.size());
            floorSerialized.dagGroups = blobs.AppendArray(plane.dag.groups);
            floorSerialized.dagMeshlets = blobs.AppendArray(plane.dag.meshlets);
            floorSerialized.dagMeshletTri = blobs.AppendArray(plane.dag.meshletTri);
            floorSerialized.dagMeshletVtx = blobs.AppendArray(plane.dag.meshletVtx);
            FBlobDeserializer des = blobs.Deserializer();
            CHECK_MSG(gpu.Upload(&des, floorSerialized, floor) == GPUScene::Result::InProgress, "floor upload rejected");
            gpu.Join();
        }

        // --- Upload the deforming grid as DYNAMIC geometry (rest pose seeds all slots) -----------
        GeometryHandle gridHandle;
        Vector<unsigned char> gridPayload(GLOBAL_ALLOC);
        FSerializedMesh gridSerialized(GLOBAL_ALLOC);
        {
            Vector<FQVertex> rest(GLOBAL_ALLOC);
            rest.reserve(gridVtxCount);
            for (GridVertex const& g : gridVertices)
                rest.push_back(EvalGridVertex(g, 0.0f));
            MemoryBlobSerializer blobs(gridPayload);
            gridSerialized.vertices = blobs.AppendArray(rest);
            gridSerialized.vertexCount = gridVtxCount;
            FSerializedMeshLOD& lod0 = gridSerialized.lods.emplace_back();
            lod0.indices = blobs.AppendArray(gridIndices);
            lod0.indexCount = gridIdxCount;
            // No DAG/meshlets: dynamic geo is drawn via the dedicated vertex/index path.
            FBlobDeserializer des = blobs.Deserializer();
            GPUScene::Result r = gpu.UploadDynamic(&des, gridSerialized, gridHandle);
            CHECK_MSG(r == GPUScene::Result::Ready, "dynamic grid upload failed ({})", r);
        }

        // --- Materials -------------------------------------------------------------------------
        Vector<GSMaterial> palette(GLOBAL_ALLOC);
        const uint32_t matFloor = static_cast<uint32_t>(palette.size());
        { GSMaterial m = BaseMat(); m.baseColorFactor = float4(0.1f, 0.6f, 0.62f, 1.0f); m.roughnessFactor = 0.9f; palette.push_back(m); }
        const uint32_t matGrid = static_cast<uint32_t>(palette.size());
        { GSMaterial m = BaseMat(); m.baseColorFactor = float4(0.25f, 0.55f, 0.9f, 1.0f); m.roughnessFactor = 0.3f; m.metallicFactor = 0.4f; palette.push_back(m); }

        UBO ubo{};
        ubo.ptDispatchTileSide = 1;
        ubo.ptSamplesPerPixel = 1;

        auto AuthorFrame = [&]
        {
            auto tables = gpu.BeginScene(2u, static_cast<uint32_t>(palette.size()), 1u);
            tables.instances[0] = InstanceDesc{.geometry = floor, .transform = float3(0.0f, -0.4f, 0.0f),
                                               .rotation = angleAxis(0.0f, float3(0, 1, 0)),
                                               .scale = float3(8.0f, 1.0f, 8.0f), .materialIndex = matFloor};
            tables.instances[1] = InstanceDesc{.geometry = gridHandle, .transform = float3(0.0f),
                                               .rotation = angleAxis(0.0f, float3(0, 1, 0)),
                                               .scale = float3(1.0f), .materialIndex = matGrid};
            for (size_t i = 0; i < palette.size(); ++i)
                tables.materials[i] = palette[i];
            GSLight& key = tables.lights[0];
            key = GSLight{};
            key.type = 4u; // Rect area light
            key.color = float3(1.0f, 0.97f, 0.92f);
            key.power = 14.0f;
            key.position = float3(0.0f, 5.0f, 0.0f);
            key.dpdu = float3(2.0f, 0.0f, 0.0f);
            key.dpdv = float3(0.0f, 0.0f, 2.0f);
            key.direction = float3(0.0f, -1.0f, 0.0f);
            key.selectionWeight = key.power;
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
        AuthorFrame();
        gpu.BuildUBO(ubo);
        ubo.ambientColor = float3(0.02f, 0.025f, 0.035f);
        ubo.ambientPower = 1.0f;
        ubo.useEnvMap = 0u;
        TextureHandle viewLUTSdrHandle{};
        TextureHandle viewLUTHdrHandle{};
        {
            ImmediateContext ctx(RHIDeviceQueueType::Compute, device.Get());
            auto* cmd = ctx.Get();
            cmd->Begin();
            auto r = gpu.BuildTLAS(cmd, /*update*/ false);
            cmd->End();
            if (r == GPUScene::TLASBuildResult::Built)
                ctx.Submit(), ctx.WaitIdle();
        }

        enum class Mode { Raster, PathTracer };
        Mode mode = Mode::PathTracer;
        bool renderPaused = false;
        RendererConfig cfg{};
        PostprocessUBO postprocessGlobals{};
        RendererOutputs handles{};
        RHIExtent2D renderExtent{};

        CSDebugTextData hud[4]{};
        hud[0].x = 16; hud[0].y = 16; hud[0].SetText("GPUScene dynamic geometry (BLAS refit)");
        hud[1].x = 16; hud[1].y = 40; hud[1].SetText("TAB renderer R refit/periodic B rebuild-every-fram F pause WASD+drag");
        hud[2].x = 16; hud[2].y = 64;
        hud[3].x = 16; hud[3].y = 88;

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
            createCSDebugTextPassBackBuffer(renderer.get(), "Debug Text", hud);
            renderer->EndSetup();
            renderExtent = extent;
        };
        auto RecreateRenderer = [&] { renderer = ConstructUnique<Renderer>(GLOBAL_ALLOC, rendererDesc, device, swapchain, GLOBAL_ALLOC); };
        BuildGraph(renderer->GetSwapchainExtent());

        FArcballCamera camera{.center = {0.0f, 0.2f, 0.0f}, .radius = 5.0f,
                              .rot = normalize(angleAxis(radians(-28.0f), float3(1, 0, 0))),
                              .zNear = 0.01f, .fovY = radians(50.0f)};
        ExampleFpsCounter fps;
        SDL_Event event{};
        uint64_t lastTicks = SDL_GetTicksNS();
        bool paused = false;
        bool rebuildEveryFrame = false;
        uint32_t cadence = 64u; // periodic rebuild cadence (0 = refit only)
        float animTime = 0.0f;

        while (!Examples_ShouldClose(window, renderer.get(), swapchain, &event))
        {
            uint64_t now = SDL_GetTicksNS();
            float dt = static_cast<float>(now - lastTicks) / 1e9f;
            lastTicks = now;

            RHIExtent2D currentExtent = renderer->GetSwapchainExtent();
            if (currentExtent.x == 0u || currentExtent.y == 0u)
                continue;
            if (currentExtent.x != renderExtent.x || currentExtent.y != renderExtent.y)
            {
                device->WaitIdle();
                RecreateRenderer();
                BuildGraph(currentExtent);
            }

            if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat)
            {
                if (event.key.key == SDLK_TAB)
                {
                    mode = mode == Mode::Raster ? Mode::PathTracer : Mode::Raster;
                    device->WaitIdle();
                    RecreateRenderer();
                    BuildGraph(renderExtent);
                    ubo.ptAccumulatedFrames = 0u;
                }
                else if (event.key.key == SDLK_F)
                    paused = !paused;
                else if (event.key.key == SDLK_R)
                {
                    cadence = cadence == 0u ? 64u : 0u;
                    rebuildEveryFrame = false;
                }
                else if (event.key.key == SDLK_B)
                    rebuildEveryFrame = !rebuildEveryFrame;
            }
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
                default: break;
                }
            }

            if (!paused)
                animTime += dt;
            camera.aspect = static_cast<float>(renderExtent.x) / static_cast<float>(renderExtent.y);
            bool cameraMoved = camera.UpdateMovement(dt);
            cameraMoved |= camera.Update(event);

            // --- Per-frame dynamic geometry update: open the window (advances the ring slot),
            //     rewrite this slot's quantized vertices (marks dirty), close the window. The graph's
            //     refit pass picks it up before the TLAS update.
            gpu.SetDynamicGeometryRebuildRate(rebuildEveryFrame ? 1u : cadence);
            gpu.BeginDynamicGeometryUpdate();
            Span<std::byte> verts = gpu.UpdateDynamicGeometry(gridHandle);
            auto* dst = reinterpret_cast<FQVertex*>(verts.data());
            for (uint32_t i = 0; i < gridVtxCount; ++i)
                dst[i] = EvalGridVertex(gridVertices[i], animTime);
            gpu.EndDynamicGeometryUpdate();

            AuthorFrame();

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

            const char* refitMode = rebuildEveryFrame ? "rebuild/frame" : (cadence == 0u ? "refit-only" : "periodic(64)");
            hud[2].SetText(fmt::format("refit {}  rebuild {}  mode {}", gpu.GetDynamicRefitCount(),
                                       gpu.GetDynamicRebuildCount(), refitMode));
            hud[3].SetText(fmt::format("{}   {:.0f} FPS{}", mode == Mode::PathTracer ? "Path Tracer" : "Raster",
                                       fps.Update(), paused ? "   [PAUSED]" : ""));

            Examples_NewFrame(renderer.get());
            if (mode == Mode::PathTracer)
                ubo.ptAccumulatedFrames += PTSamplesPerDispatch(ubo);
            if (cameraMoved || !paused)
                ubo.ptAccumulatedFrames = 0;
        }
    }
    Examples_DestroyVulkan(window, renderer.release(), app, device, swapchain);
    return 0;
}
