// libsm64 Demo
// Demonstrates 'Mario 64 as a library' in Foundation. And a small CPU SW raster and some tricks for texture baking to avoid using vertex colors in the pathtracer.
// Requires pre-built libsm64 binaries! Get yours at https://nightly.link/libsm64/libsm64/workflows/main/master
// And of course, ROM file for the US version. Rename it to "baserom.us.z64" and place it besides the built executable with libsm64.

#include <SDL3/SDL.h>
#define SM64_ROM_NAME "baserom.us.z64"
#if defined(_WIN32)
#define SM64_LIB_NAME "sm64.dll"
#elif defined(__APPLE__)
#define SM64_LIB_NAME "libsm64.dylib"
#else
#define SM64_LIB_NAME "libsm64.so"
#endif

#include "LevelCollision.h"
#include "libsm64.h"

#include <Math/ModelViewProjection.hpp>
#include <Renderer/Mesh.hpp>
#include <Renderer/ProgressivePathtracer.hpp>
#include <Renderer/Rasterizer.hpp>
#include <Renderer/Renderer.hpp>
#include <Renderer/Texture.hpp>
#include "../Examples.hpp"

template <typename T>
using SMVector = std::vector<T, StlDefaultAllocator<T>>;
namespace
{
    using namespace Foundation::Math;
    struct LibSM64
    {
        void (*global_init)(const uint8_t* rom, uint8_t* outTexture);
        void (*global_terminate)(void);
        void (*audio_init)(const uint8_t* rom);
        uint32_t (*audio_tick)(uint32_t numQueuedSamples, uint32_t numDesiredSamples, int16_t* audio_buffer);
        void (*play_music)(uint8_t player, uint16_t seqArgs, uint16_t fadeTimer);
        void (*static_surfaces_load)(const SM64Surface* surfaceArray, uint32_t numSurfaces);
        int32_t (*mario_create)(float x, float y, float z);
        void (*mario_tick)(int32_t marioId, const SM64MarioInputs* inputs, SM64MarioState* outState,
                           SM64MarioGeometryBuffers* outBuffers);
        void (*mario_delete)(int32_t marioId);
    } g_sm64{};

    static SDL_SharedObject* g_libModule = nullptr;

    static bool Resolve(SDL_SharedObject* mod, void*& out, const char* name)
    {
        out = reinterpret_cast<void*>(SDL_LoadFunction(mod, name));
        return out != nullptr;
    }

    static void LoadLibSM64(const char* dllPath)
    {
        g_libModule = SDL_LoadObject(dllPath);
        CHECK(g_libModule != nullptr);
        CHECK(Resolve(g_libModule, *(void**)&g_sm64.global_init, "sm64_global_init"));
        CHECK(Resolve(g_libModule, *(void**)&g_sm64.global_terminate, "sm64_global_terminate"));
        CHECK(Resolve(g_libModule, *(void**)&g_sm64.audio_init, "sm64_audio_init"));
        CHECK(Resolve(g_libModule, *(void**)&g_sm64.audio_tick, "sm64_audio_tick"));
        CHECK(Resolve(g_libModule, *(void**)&g_sm64.play_music, "sm64_play_music"));
        CHECK(Resolve(g_libModule, *(void**)&g_sm64.static_surfaces_load, "sm64_static_surfaces_load"));
        CHECK(Resolve(g_libModule, *(void**)&g_sm64.mario_create, "sm64_mario_create"));
        CHECK(Resolve(g_libModule, *(void**)&g_sm64.mario_tick, "sm64_mario_tick"));
        CHECK(Resolve(g_libModule, *(void**)&g_sm64.mario_delete, "sm64_mario_delete"));
    }
    static void FreeLibSM64() { SDL_UnloadObject(g_libModule); }

    constexpr uint32_t kMaxTris = SM64_GEO_MAX_TRIANGLES;
    constexpr uint32_t kMaxVerts = kMaxTris * 3u;
    constexpr uint32_t kMaxIdx = kMaxVerts;
    constexpr float kMarioTickStep = 1.0f / 30.0f; // libsm64 advances ~1/30s per mario_tick

    inline uint8_t ToColor8(float c) { return static_cast<uint8_t>(roundf(clamp(c, 0.0f, 1.0f) * 255.0f)); }
    inline uint32_t PackColor32(const float3& c)
    {
        return (static_cast<uint32_t>(ToColor8(c.x)) << 16) | (static_cast<uint32_t>(ToColor8(c.y)) << 8) |
            (static_cast<uint32_t>(ToColor8(c.z)));
    }

    static void BuildPalette(SMVector<float3>& colors, SM64MarioGeometryBuffers const& sgeo)
    {
        const uint32_t used = static_cast<uint32_t>(sgeo.numTrianglesUsed) * 3u;
        for (uint32_t i = 0; i < used; ++i)
        {
            // NOTE: UV==1 vertices are colored with vertex color only
            if (sgeo.uv[2u * i + 0] != 1.0f || sgeo.uv[2u * i + 1] != 1.0f)
                continue;
            const float3 c = float3(sgeo.color[3u * i + 0], sgeo.color[3u * i + 1], sgeo.color[3u * i + 2]);
            colors.push_back(c);
        }

        auto comp = [](const float3& a, const float3& b) { return PackColor32(a) < PackColor32(b); };
        auto eq = [](const float3& a, const float3& b) { return PackColor32(a) == PackColor32(b); };

        std::sort(colors.begin(), colors.end(), comp);
        colors.erase(std::unique(colors.begin(), colors.end(), eq), colors.end());
    }
    // Rasterize tris that's both texture-mapped, and vertex-colored, into a temporary texture
    // @ref PreprocessMarioTexture will blend it to the original texture so we can work without using vertex colors
    // (1/2)
    // As to why - with [...]Pathtracers and our uber-material model, extending it to support beyond PBR properties isn't very
    // feasible. Note that it can still be done in Rasterizer through @ref RasterFeature with a custom rendering pass.
    static void PreprocessMarioVertexColor(uint8_t* tempTex, uint32_t width, uint32_t height,
                                           const SM64MarioGeometryBuffers& sgeo)
    {
        // Bake vertex colors and blend it to tex
        const uint32_t numTris = static_cast<uint32_t>(sgeo.numTrianglesUsed);
        for (uint32_t t = 0; t < numTris; ++t)
        {
            float2 uvs[3];
            float3 cols[3];
            for (uint32_t v = 0; v < 3; ++v)
            {
                uint32_t i = t * 3 + v;
                uvs[v] = float2(sgeo.uv[2 * i + 0], sgeo.uv[2 * i + 1]);
                cols[v] = float3(sgeo.color[3 * i + 0], sgeo.color[3 * i + 1], sgeo.color[3 * i + 2]);
            }

            // Coordinates in pixel space
            float2 p0 = uvs[0] * float2(static_cast<float>(width), static_cast<float>(height));
            float2 p1 = uvs[1] * float2(static_cast<float>(width), static_cast<float>(height));
            float2 p2 = uvs[2] * float2(static_cast<float>(width), static_cast<float>(height));

            // Bounding box
            int minX = std::max(0, static_cast<int>(std::floor(std::min({p0.x, p1.x, p2.x}))));
            int minY = std::max(0, static_cast<int>(std::floor(std::min({p0.y, p1.y, p2.y}))));
            int maxX = std::min(static_cast<int>(width - 1), static_cast<int>(std::ceil(std::max({p0.x, p1.x, p2.x}))));
            int maxY =
                std::min(static_cast<int>(height - 1), static_cast<int>(std::ceil(std::max({p0.y, p1.y, p2.y}))));

            auto edge = [](const float2& a, const float2& b, const float2& c)
            { return (c.x - a.x) * (b.y - a.y) - (c.y - a.y) * (b.x - a.x); };

            float area = edge(p0, p1, p2);
            if (std::abs(area) < 1e-5f)
                continue;

            // We want a conservative raster, that is if an edge partially covers a pixel w/o fully containing it,
            // these should be contained as well.
            // * Half-plane tests work with the center of the pixel
            // * Inluding the corners would be what we want. This is done by shifting by pixel's half-extends.
            // * Typically, you'd do that w.r.t the edge's normal. For us the following is used:
            // * On both axes:
            //   - d_w0_dx = dx * |p2.y - p1.y|, d_w0_dy = dy * |p2.x - p1.x|, and so on
            //   - We're already in pixel space, so dx = dy = 0.5f is half-extends.
            //   - Let's use a L1/Hamming distance here for simplicity. Note that this produces a overestimated area.
            float e0 = 0.5f * (std::abs(p2.y - p1.y) + std::abs(p2.x - p1.x));
            float e1 = 0.5f * (std::abs(p0.y - p2.y) + std::abs(p0.x - p2.x));
            float e2 = 0.5f * (std::abs(p1.y - p0.y) + std::abs(p1.x - p0.x));
            // * See also
            //   https://developer.nvidia.com/gpugems/gpugems2/part-v-image-oriented-computing/chapter-42-conservative-rasterization

            for (int y = minY; y <= maxY; ++y)
            {
                for (int x = minX; x <= maxX; ++x)
                {
                    float2 p(x + 0.5f, y + 0.5f);
                    float w0 = edge(p1, p2, p);
                    float w1 = edge(p2, p0, p);
                    float w2 = edge(p0, p1, p);

                    // Inside/outside test
                    if (area > 0.0f)
                    {
                        if (w0 + e0 < 0.0f || w1 + e1 < 0.0f || w2 + e2 < 0.0f)
                            continue;
                    }
                    else
                    {
                        if (w0 - e0 > 0.0f || w1 - e1 > 0.0f || w2 - e2 > 0.0f)
                            continue;
                    }

                    float sign = area > 0.0f ? 1.0f : -1.0f;
                    // Barycentric weights
                    float bw0 = std::max(0.0f, w0 * sign);
                    float bw1 = std::max(0.0f, w1 * sign);
                    float bw2 = std::max(0.0f, w2 * sign);
                    float bSum = bw0 + bw1 + bw2;
                    // Normalize
                    bw0 /= bSum;
                    bw1 /= bSum;
                    bw2 /= bSum;
                    // Interpolate
                    float3 c = cols[0] * bw0 + cols[1] * bw1 + cols[2] * bw2;

                    uint32_t o = (y * width + x) * 4u;
                    tempTex[o + 0] = ToColor8(c.x);
                    tempTex[o + 1] = ToColor8(c.y);
                    tempTex[o + 2] = ToColor8(c.z);
                    tempTex[o + 3] = 255;
                }
            }
        }
    }

    // Blend vertex color tex and do some UV remapping trickery
    // With it vertices with only vertex colors will also work without using them, thereby remapping it to a
    // pre-processed palette instead (2/2)
    static void PreprocessMarioTexture(uint8_t* texData, uint32_t width, uint32_t height,
                                       const SMVector<float3>& palette, const uint8_t* vcolorTex)
    {
        for (uint32_t i = 0; i < width * height; ++i)
        {
            uint32_t o = i * 4u;
            uint32_t a = texData[o + 3];
            texData[o + 0] = static_cast<uint8_t>(
                (static_cast<uint32_t>(texData[o + 0]) * a + static_cast<uint32_t>(vcolorTex[o + 0]) * (255 - a)) /
                255);
            texData[o + 1] = static_cast<uint8_t>(
                (static_cast<uint32_t>(texData[o + 1]) * a + static_cast<uint32_t>(vcolorTex[o + 1]) * (255 - a)) /
                255);
            texData[o + 2] = static_cast<uint8_t>(
                (static_cast<uint32_t>(texData[o + 2]) * a + static_cast<uint32_t>(vcolorTex[o + 2]) * (255 - a)) /
                255);
            texData[o + 3] = 255;
        }
        // Pack vertex colors into top row
        // @ref PackVertices will map them to UVs there
        for (uint32_t i = 0; i < palette.size(); ++i)
        {
            const float3 c = palette[i];
            const uint32_t o = (0u * width + i) * 4u; // row 0, left to right
            texData[o + 0] = ToColor8(c.x);
            texData[o + 1] = ToColor8(c.y);
            texData[o + 2] = ToColor8(c.z);
            texData[o + 3] = 255;
        }
    }

    // Preprocess vertices
    static void PackVertices(SMVector<FQVertex>& verts, const SM64MarioGeometryBuffers& geo,
                             const SMVector<float>& posBuf, const SMVector<float>& norBuf, const SMVector<float>& uvBuf,
                             const SMVector<float>& colBuf, const SMVector<float3>& paletteColors)
    {
        auto comp = [](const float3& a, const float3& b) { return PackColor32(a) < PackColor32(b); };
        const uint32_t used = static_cast<uint32_t>(geo.numTrianglesUsed) * 3u;
        for (uint32_t i = 0; i < used; ++i)
        {
            FVertex v{};
            v.position = float3(posBuf[3u * i + 0], posBuf[3u * i + 1], posBuf[3u * i + 2]);
            v.normal = float3(norBuf[3u * i + 0], norBuf[3u * i + 1], norBuf[3u * i + 2]);
            const float su = uvBuf[2u * i + 0], sv = uvBuf[2u * i + 1];
            if (su != 1.0f || sv != 1.0f) // Actually texture sampled
            {
                v.uv = float2(su, sv);
            }
            else
            {
                // These would read from vertex colors, but we have never supported those
                // Thus remapping ourselves to a pre-processed pallete instead
                const float3 c = float3(colBuf[3u * i + 0], colBuf[3u * i + 1], colBuf[3u * i + 2]);
                auto it = std::lower_bound(paletteColors.begin(), paletteColors.end(), c, comp);
                uint32_t pi = 0;
                if (it != paletteColors.end() && PackColor32(*it) == PackColor32(c))
                    pi = static_cast<uint32_t>(std::distance(paletteColors.begin(), it));

                v.uv = float2((static_cast<float>(pi) + 0.5f) / static_cast<float>(SM64_TEXTURE_WIDTH),
                              0.5f / static_cast<float>(SM64_TEXTURE_HEIGHT));
            }
            verts[i] = FQVertex::Pack(v);
        }
        // Degenerate leftover triangles (zero-area) so they don't rasterize. The BLAS just refits.
        for (uint32_t i = used; i < kMaxVerts; ++i)
        {
            FVertex v{};
            v.position = float3(0, 0, 0);
            v.normal = float3(0, 0, 1);
            v.uv = float2(0, 0);
            verts[i] = FQVertex::Pack(v);
        }
    }

    // Level geometry
    static FImportedMesh BuildLevelMesh(Allocator* alloc)
    {
        FImportedMesh mesh(alloc);
        const uint32_t n = static_cast<uint32_t>(surfaces_count);
        mesh.vertices.resize(n * 3u);
        mesh.lods.emplace_back(alloc);
        mesh.lods[0].indices.resize(n * 3u);
        for (uint32_t t = 0; t < n; ++t)
        {
            const SM64Surface& s = surfaces[t];
            const float3 p0(s.vertices[0][0], s.vertices[0][1], s.vertices[0][2]);
            const float3 p1(s.vertices[1][0], s.vertices[1][1], s.vertices[1][2]);
            const float3 p2(s.vertices[2][0], s.vertices[2][1], s.vertices[2][2]);
            const float3 nrm = normalize(cross(p1 - p0, p2 - p0));
            const uint32_t base = t * 3u;
            const float3 pts[3] = {p0, p1, p2};
            for (uint32_t k = 0; k < 3u; ++k)
            {
                FVertex v{};
                v.position = pts[k];
                v.normal = nrm;
                v.uv = float2(0.0f, 0.0f);
                // Quantization implicitly builds tangent frames
                mesh.vertices[base + k] = v;
            }
            mesh.lods[0].indices[base + 0] = base + 0;
            mesh.lods[0].indices[base + 1] = base + 1;
            mesh.lods[0].indices[base + 2] = base + 2;
        }
        mesh.Optimize();
        mesh.ClusterizeDAG();
        mesh.Quantize();
        return mesh;
    }


    // Inputs
    static SDL_Gamepad* gGamepad = nullptr;

    static float ReadAxis(int16_t v)
    {
        float r = static_cast<float>(v) / 32767.0f;
        if (r < 0.2f && r > -0.2f)
            return 0.0f;
        return r > 0.0f ? (r - 0.2f) / 0.8f : (r + 0.2f) / 0.8f;
    }

    static void OpenFirstGamepad()
    {
        int count = 0;
        SDL_JoystickID* pads = SDL_GetGamepads(&count);
        if (pads && count > 0)
            gGamepad = SDL_OpenGamepad(pads[0]);
        SDL_free(pads);
    }

    static void OnSdlEvent(SDL_Event* e)
    {
        CHECK(e != nullptr);
        if (e->type == SDL_EVENT_GAMEPAD_ADDED && !gGamepad)
            gGamepad = SDL_OpenGamepad(e->jdevice.which);
        else if (e->type == SDL_EVENT_GAMEPAD_REMOVED && gGamepad && SDL_GetGamepadID(gGamepad) == e->jdevice.which)
        {
            SDL_CloseGamepad(gGamepad);
            gGamepad = nullptr;
        }
    }

    struct FMarioCamera
    {
        float3 center{};
        float yaw = 0.0f;
        float radius = 1000.0f;
        float height = 200.0f;
        float fovY = radians(45.0f);
        float zNear = 1.0f;
        float aspect = 1.0f;
        mat4 view{};
        mat4 proj{};
        float3 position{};
        float3 forward{};

        void RefreshMatrices()
        {
            position = center + float3(radius * cosf(yaw), height, radius * sinf(yaw));
            forward = normalize(center - position);
            const float3 backward = -forward; // camera +Z (outward)
            const float3 right = normalize(cross(float3(0.0f, 1.0f, 0.0f), backward));
            const float3 up = cross(backward, right);
            const quat rot = quat_cast(mat3{right, up, backward});
            view = viewMatrixRHReverseZ(position, rot);
            proj = infinitePerspectiveRHReverseZ(fovY, aspect, zNear);
        }
    };

    static void GatherInputs(SM64MarioInputs& inputs, float& camYawDelta, const FMarioCamera& camera)
    {
        const bool* kbd = SDL_GetKeyboardState(NULL);

        inputs.stickX = (kbd[SDL_SCANCODE_D] ? 1.0f : 0.0f) - (kbd[SDL_SCANCODE_A] ? 1.0f : 0.0f);
        inputs.stickY = (kbd[SDL_SCANCODE_S] ? 1.0f : 0.0f) - (kbd[SDL_SCANCODE_W] ? 1.0f : 0.0f);
        inputs.buttonA = (kbd[SDL_SCANCODE_SPACE] || kbd[SDL_SCANCODE_X]) ? 1u : 0u;
        inputs.buttonB = (kbd[SDL_SCANCODE_LSHIFT] || kbd[SDL_SCANCODE_C]) ? 1u : 0u;
        inputs.buttonZ = kbd[SDL_SCANCODE_Z] ? 1u : 0u;
        if (gGamepad)
        {
            const float lx = ReadAxis(SDL_GetGamepadAxis(gGamepad, SDL_GAMEPAD_AXIS_LEFTX));
            const float ly = ReadAxis(SDL_GetGamepadAxis(gGamepad, SDL_GAMEPAD_AXIS_LEFTY));
            const float rx = ReadAxis(SDL_GetGamepadAxis(gGamepad, SDL_GAMEPAD_AXIS_RIGHTX));
            if (fabsf(lx) > 0.01f)
                inputs.stickX = lx;
            if (fabsf(ly) > 0.01f)
                inputs.stickY = ly;
            if (SDL_GetGamepadButton(gGamepad, SDL_GAMEPAD_BUTTON_SOUTH))
                inputs.buttonA = 1u;
            if (SDL_GetGamepadButton(gGamepad, SDL_GAMEPAD_BUTTON_EAST))
                inputs.buttonB = 1u;
            if (SDL_GetGamepadButton(gGamepad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER))
                inputs.buttonZ = 1u;
            camYawDelta = rx;
        }

        if (kbd[SDL_SCANCODE_LEFT])
            camYawDelta -= 1.0f;
        if (kbd[SDL_SCANCODE_RIGHT])
            camYawDelta += 1.0f;

        inputs.camLookX = -camera.radius * cosf(camera.yaw);
        inputs.camLookZ = -camera.radius * sinf(camera.yaw);
    }

    // To GPUScene!
    void CommitMarioScene(GPUScene& gpu, GeometryHandle level, GeometryHandle mario, TextureHandle marioTexture,
                          RendererUBO& ubo)
    {
        auto tables = gpu.BeginScene(2, 2, 2);
        tables.materials[0] = GSMaterial{};
        tables.materials[0].baseColorFactor = float4(1.0f, 1.0f, 1.0f, 1.0f);
        tables.materials[0].metallicFactor = 0.0f;
        tables.materials[0].roughnessFactor = 0.75f;
        tables.materials[0].ior = 1.5f;
        tables.materials[1] = tables.materials[0];
        tables.materials[1].baseColorFactor = float4(1.0f, 1.0f, 1.0f, 1.0f);
        if (marioTexture.IsValid())
            tables.materials[1].baseColorTexture = marioTexture.index;
        tables.instances[0] = GSInstance{
            .transform = float3(0, 0, 0),
            .rotation = quat(0, 0, 0, 1),
            .scale = float3(1, 1, 1),
            .materialIndex = 1,
            .resourceIndex = mario.index,
            .type = kGSInstanceTypeMesh,
        };
        tables.instances[1] = GSInstance{
            .transform = float3(0, 0, 0),
            .rotation = quat(0, 0, 0, 1),
            .scale = float3(1, 1, 1),
            .materialIndex = 0,
            .resourceIndex = level.index,
            .type = kGSInstanceTypeMesh,
        };
        tables.lights[0] = GSLight{.flags = kGSLightTypeEnvironment, .color = float3(0.5f, 0.6f, 0.8f), .power = 1.0f};
        tables.lights[1] = GSLight{.flags = kGSLightTypeDirectional | to_integer(GSLightFlagsBits::UseShadow),
                                   .color = float3(1.0f, 0.96f, 0.9f),
                                   .power = 1.0f,
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
        ubo.ptMaxBounces = 2u;
        auto resources = CreateGPUSceneRendererResources(ctx.renderer.get(), &gpu);
        BuildGPUSceneHostUpdatePass(ctx.renderer.get(), resources);
        Example_BuildExampleRenderer(renderer, ctx.renderer.get(), &ubo, resources, cfg, outputs);
        Examples_BuildTonemappingPass(ctx.renderer.get(), outputs, true);
        RenderUtils::createCSDebugTextPassBackBuffer(ctx.renderer.get(), "Debug Text", Examples_HudLines(input));
        ctx.renderer->EndSetup();
    }
} // namespace

int main(int argc, char** argv)
{
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO);
    OpenFirstGamepad();
    SDL_Window* window = SDL_CreateWindow(FOUNDATION_APPLICATION_TITLE("libsm64 Mario (CPU dynamic mesh)"), 1280, 720,
                                          Examples_SDLWindowFlagsVulkan);
    auto ctx = Examples_InitVulkan(window, argc, argv, RendererDesc{});
    LoadLibSM64(ctx.app->ResolveRelativePathBase(SM64_LIB_NAME).c_str());
    void* sm64_rom;
    {
        auto file = ctx.app->ResolveRelativePathBase(SM64_ROM_NAME);
        auto info = ctx.app->QueryFileInfo(file);

        FILE* romFile = fopen(file.c_str(), "rb");
        CHECK(info && romFile != nullptr);
        sm64_rom = GLOBAL_ALLOC->Allocate(info->size);
        fread(sm64_rom, 1, info->size, romFile);
        fclose(romFile);
    }
    void* sm64_tex = GLOBAL_ALLOC->Allocate(SM64_TEXTURE_WIDTH * SM64_TEXTURE_HEIGHT * 4);
    g_sm64.global_init(static_cast<uint8_t*>(sm64_rom), static_cast<uint8_t*>(sm64_tex));
    g_sm64.audio_init(static_cast<uint8_t*>(sm64_rom));
    g_sm64.static_surfaces_load(surfaces, static_cast<uint32_t>(surfaces_count));

    SDL_AudioSpec audioSpec{};
    audioSpec.freq = 32000;
    audioSpec.format = SDL_AUDIO_S16;
    audioSpec.channels = 2;
    SDL_AudioStream* audioStream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &audioSpec, nullptr, nullptr);
    CHECK(audioStream != nullptr);
    SDL_ResumeAudioDevice(SDL_GetAudioStreamDevice(audioStream));

    int32_t marioId = g_sm64.mario_create(0.0f, 1000.0f, 0.0f);
    CHECK(marioId >= 0);

    g_sm64.play_music(0, 0x05 | 0x80, 0);

    // Gemoetry states
    // These are always morphed on the CPU
    SMVector<float> posBuf(9u * kMaxTris), norBuf(9u * kMaxTris), uvBuf(6u * kMaxTris), colBuf(9u * kMaxTris);
    SM64MarioState state{};
    SM64MarioGeometryBuffers geo{};
    geo.position = posBuf.data();
    geo.normal = norBuf.data();
    geo.uv = uvBuf.data();
    geo.color = colBuf.data();
    // Input states
    SM64MarioInputs inputs{};

    // GPUScene init
    GPUSceneDesc desc{};
    desc.primitiveBudget = 1024u * 1024u;
    desc.dynamicGeometryBudget = 1024u * 1024u;
    desc.dynamicStagingBudget = 1024u * 1024u;
    desc.instanceBudget = 8;
    desc.materialBudget = 8;
    desc.lightBudget = 8;
    desc.geometryBudget = 8;
    desc.tlasInstanceBudget = 8;
    GPUScene gpu(ctx.device.Get(), ctx.jobs.get(), GLOBAL_ALLOC, desc);

    GeometryHandle mario{};
    CHECK(gpu.Allocate(kMaxVerts, kMaxIdx, mario, /*isGpu=*/false) == GPUScene::Result::Ready);

    // Upload static level geometry
    GeometryHandle level{};
    {
        FImportedMesh levelMesh = BuildLevelMesh(GLOBAL_ALLOC);
        CHECK(gpu.Upload(levelMesh, level) == GPUScene::Result::Ready);
    }

    // Warm up to get initial geos
    SMVector<uint8_t> vcolorTex(SM64_TEXTURE_WIDTH * SM64_TEXTURE_HEIGHT * 4, 255);
    for (int i = 0; i < 30; ++i)
    {
        // XXX: Hopefully this picks up all UV animations we'd use. See @ref PreprocessMarioVertexColor
        g_sm64.mario_tick(marioId, &inputs, &state, &geo);
        PreprocessMarioVertexColor(vcolorTex.data(), SM64_TEXTURE_WIDTH, SM64_TEXTURE_HEIGHT, geo);
    }

    // Preprocess vertices
    // See @ref PreprocessMarioTexture.
    SMVector<float3> paletteColors;
    BuildPalette(paletteColors, geo);
    SMVector<FQVertex> verts(kMaxVerts);
    SMVector<uint32_t> indices(kMaxIdx);
    for (uint32_t i = 0; i < kMaxIdx; ++i)
        indices[i] = i;
    PackVertices(verts, geo, posBuf, norBuf, uvBuf, colBuf, paletteColors);

    TextureHandle marioTexHandle{};
    {
        FTexture marioTex(GLOBAL_ALLOC);
        marioTex.Initialize(RHIResourceFormat::R8G8B8A8Srgb, RHITextureDimension::E2D, SM64_TEXTURE_WIDTH,
                            SM64_TEXTURE_HEIGHT);
        const size_t texelCount = static_cast<size_t>(SM64_TEXTURE_WIDTH) * SM64_TEXTURE_HEIGHT;
        auto* u8tex = static_cast<uint8_t*>(sm64_tex);
        marioTex.bytes.assign(u8tex, u8tex + texelCount * 4u);
        PreprocessMarioTexture(marioTex.bytes.data(), SM64_TEXTURE_WIDTH, SM64_TEXTURE_HEIGHT, paletteColors,
                               vcolorTex.data());
        CHECK(gpu.Upload(marioTex, marioTexHandle) == GPUScene::Result::Ready);
    }

    RendererUBO ubo{};
    RendererConfig cfg{.cullFlags{CullFlagsBits::Frustum | CullFlagsBits::Backface}};
    if (!ctx.device->GetCapabilities().raytracingInline)
        cfg.viewFlags &= ~ViewFlagsBits::EnableRasterRTShadows;

    FMarioCamera camera{};
    RendererOutputs outputs{};
    ExampleInputState input{};
    ExampleFpsCounter fps{};
    ExampleRenderer renderer = ExampleRenderer::Raster;
    uint64_t t0 = SDL_GetTicksNS();

    // Interpoalted ticks, see @ref kMarioTickStep usage below...
    float tick = 0.0f;
    float3 lastPos = {0.0f, 0.0f, 0.0f}, currPos = {0.0f, 0.0f, 0.0f};
    SMVector<float> lastGeoPos(9u * kMaxTris), currGeoPos(9u * kMaxTris);

    uint32_t ticks = 0;
    while (true)
    {
        uint64_t t1 = SDL_GetTicksNS();
        float dt = static_cast<float>(t1 - t0) / 1e9f;
        t0 = t1;

        Examples_BeginFrameInput(input);
        if (Examples_PollEvents(window, ctx, input, nullptr, OnSdlEvent))
            break;

        if (input.wantResizeOrRebuild)
        {
            input.wantResizeOrRebuild = false;
            RebuildGraph(ctx, ubo, gpu, cfg, outputs, input, renderer);
        }


        float camYawDelta = 0.0f;
        GatherInputs(inputs, camYawDelta, camera);

        // Interpolate 30Hz ticks to refresh
        // libsm64 only does it at that rate...
        tick += dt;
        while (tick >= kMarioTickStep)
        {
            lastPos = currPos;
            lastGeoPos = currGeoPos;

            tick -= kMarioTickStep;
            ticks++;
            g_sm64.mario_tick(marioId, &inputs, &state, &geo);

            int16_t audioBuffer[544 * 2 * 2];
            uint32_t numSamples = g_sm64.audio_tick(SDL_GetAudioStreamAvailable(audioStream) / 4, 1100, audioBuffer);
            if (SDL_GetAudioStreamAvailable(audioStream) / 4 < 6000)
                SDL_PutAudioStreamData(audioStream, audioBuffer, numSamples * 2 * 4);

            currPos = float3(state.position[0], state.position[1], state.position[2]);
            currGeoPos = posBuf;
        }

        const float alpha = tick / kMarioTickStep;
        for (uint32_t i = 0; i < 9u * kMaxTris; ++i)
            posBuf[i] = mix(lastGeoPos[i], currGeoPos[i], alpha);
        state.position[0] = mix(lastPos.x, currPos.x, alpha);
        state.position[1] = mix(lastPos.y, currPos.y, alpha);
        state.position[2] = mix(lastPos.z, currPos.z, alpha);

        PackVertices(verts, geo, posBuf, norBuf, uvBuf, colBuf, paletteColors);

        gpu.BeginDynamicGeometryUpdate();
        gpu.UpdateDynamicGeometryCPU(mario, Span<const FQVertex>{verts.data(), verts.size()},
                                     ticks == 0
                                         ? /* needs a rebuild */ Span<const uint32_t>{indices.data(), indices.size()}
                                         : /* no topo change (indices) thus we can refit */ Span<const uint32_t>{});
        gpu.EndDynamicGeometryUpdate();
        camera.yaw += camYawDelta * dt * 2.0f;
        if (camYawDelta != 0.0f)
            ubo.ptAccumulatedFrames = 0u;
        camera.center = float3(state.position[0], state.position[1], state.position[2]);
        camera.aspect = static_cast<float>(cfg.renderExtent.x) / static_cast<float>(cfg.renderExtent.y);
        camera.RefreshMatrices();

        UpdateRendererCameraUBO(ubo, ctx.renderer->GetFrame(), camera.view, camera.proj);
        ubo.zNear = camera.zNear;
        ubo.projPlanes = planeSymmetric(camera.proj);
        ubo.camPosition = float4(camera.position, 0.0f);
        ubo.camDirection = float4(camera.forward, 0.0f);
        ubo.dbgViewFlags = cfg.viewFlags;
        ubo.dbgMaterialFlags = cfg.materialFlags;
        CommitMarioScene(gpu, level, mario, marioTexHandle, ubo);

        Examples_Text(input,
                      Format("libsm64 Mario | {:.0f} FPS | tris {} | refit {} rebuild {}", fps.Update(),
                             static_cast<uint32_t>(geo.numTrianglesUsed), gpu.GetDynamicRefitCount(),
                             gpu.GetDynamicRebuildCount()));
        Examples_Text(input, Format("Game tick: {}, Frame: {}", ticks, ctx.renderer->GetFrame()));
        Examples_Text(input, "WASD/LS move | Space/X jump | LShift/C action | Z/L1 crouch | Arrows/RS orbit");
        if (gGamepad)
            Examples_Text(input, Format("{} connected", SDL_GetGamepadNameForID(SDL_GetGamepadID(gGamepad))));
        if (Examples_RendererSwitchButton(input, renderer))
            input.wantResizeOrRebuild = true;

        Examples_NewFrame(window, ctx);
    }

    if (gGamepad)
        SDL_CloseGamepad(gGamepad);
    SDL_DestroyAudioStream(audioStream);
    g_sm64.mario_delete(marioId);
    g_sm64.global_terminate();
    FreeLibSM64();
    GLOBAL_ALLOC->Deallocate(sm64_rom);
    GLOBAL_ALLOC->Deallocate(sm64_tex);
    Examples_DestroyVulkan(window, ctx);
    return 0;
}
