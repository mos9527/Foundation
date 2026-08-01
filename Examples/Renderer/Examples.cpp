#define SDL_MAIN_HANDLED
#define FOUNDATION_EXAMPLES_IMPLEMENTATION
#include "Examples.hpp"
#include <Renderer/Rasterizer.hpp>
#include <Renderer/ProgressivePathtracer.hpp>
#include <Renderer/RealtimePathtracer.hpp>
#include <Renderer/Postprocess.hpp>

bool Examples_RendererSwitchButton(ExampleInputState& input, ExampleRenderer& currentRenderer)
{
    bool changed = false;    
    if (Examples_Button(input, Format(">> Current Renderer: [{}] <<", currentRenderer)))
    {
        currentRenderer = static_cast<ExampleRenderer>((static_cast<uint32_t>(currentRenderer) + 1) %
                                                       static_cast<uint32_t>(ExampleRenderer::Count));
        changed = true;
    }
    return changed;
}

bool Examples_BitmaskOptionPicker(ExampleInputState& input, unsigned& value, const char* const* labels,
                                  const unsigned* masks, unsigned count, bool solo)
{
    bool any = false;
    for (unsigned i = 0; i < count; ++i)
    {
        bool const selected = (value & masks[i]) != 0u;
        if (i > 0)
            Examples_SameLine(input, 8);
        if (Examples_Button(input, Format("{} {}", selected ? "[x]" : "[ ]", labels[i])))
        {
            if (!selected)
            {
                value |= masks[i];
                if (solo)
                {
                    for (unsigned j = 0; j < count; ++j)
                    {
                        if (j != i)
                            value &= ~masks[j];
                    }
                }
            }
            else
                value &= ~masks[i];
            any = true;
        }
    }
    return any;
}

bool Examples_BitmaskCycleButton(ExampleInputState& input, StringView label, unsigned& value,
                                 const char* const* labels, const unsigned* masks, unsigned count)
{
    int selected = -1;
    for (unsigned i = 0; i < count; ++i)
    {
        if ((value & masks[i]) != 0u)
        {
            selected = static_cast<int>(i);
            break;
        }
    }
    char const* name = selected < 0 ? "Off" : labels[selected];
    if (!Examples_Button(input, Format(">> {}: [{}] <<", label, name)))
        return false;

    for (unsigned i = 0; i < count; ++i)
        value &= ~masks[i];
    int const next = selected + 1;
    if (next >= 0 && next < static_cast<int>(count))
        value |= masks[next];
    return true;
}

namespace
{
    struct ExamplePostprocessPushConstants
    {
        uint32_t viewFlags;
        uint32_t accumulatedFrames;
    };

    constexpr ViewFlagsBits kDebugViewFlags = ViewFlagsBits::BaseColor | ViewFlagsBits::Normal |
        ViewFlagsBits::Position | ViewFlagsBits::TextureLOD | ViewFlagsBits::SHARCGrid |
        ViewFlagsBits::SHARCOccupancy | ViewFlagsBits::SHARCRadiance;
    constexpr ViewFlagsBits kAovViewFlags =
        ViewFlagsBits::AOVDiffuse | ViewFlagsBits::AOVSpecular | ViewFlagsBits::AOVSampleCount;
}

bool Examples_RendererFlagsControls(ExampleInputState& input, ExampleRenderer renderer, RendererConfig& cfg)
{
    bool changed = false;

    if (IsRaster(renderer))
    {
        {
            const char* names[] = {"Overdraw", "Meshlet", "Matcap"};
            const ViewFlagsBits values[] = {ViewFlagsBits::Overdraw, ViewFlagsBits::Meshlet, ViewFlagsBits::Matcap};
            changed |= Examples_BitmaskCycleButton(input, "Raster Debug", cfg.viewFlags, names, values);
        }
        {
            const char* items[] = {"RT Shadows"};
            const ViewFlagsBits values[] = {ViewFlagsBits::EnableRasterRTShadows};
            changed |= Examples_BitmaskOptionPicker(input, cfg.viewFlags, items, values);
        }
        {
            const char* items[] = {"Frustum", "Occlusion"};
            const CullFlagsBits values[] = {CullFlagsBits::Frustum, CullFlagsBits::Occlusion};
            changed |= Examples_BitmaskOptionPicker(input, cfg.cullFlags, items, values);
        }
    }

    if (IsPathTracer(renderer))
    {
        const char* items[] = {"Diffuse", "Specular", "Sample Count"};
        const ViewFlagsBits values[] = {ViewFlagsBits::AOVDiffuse, ViewFlagsBits::AOVSpecular,
                                        ViewFlagsBits::AOVSampleCount};
        if (Examples_BitmaskCycleButton(input, "AOV View", cfg.viewFlags, items, values))
        {
            cfg.viewFlags &= ~kDebugViewFlags;
            changed = true;
        }
    }

    {
        bool debugViewChanged = false;
        if (IsRealtimePT(renderer))
        {
            const char* items[] = {"BaseColor", "Normal", "Position", "Texture LOD", "SHARC Grid",
                                   "SHARC Occupancy", "SHARC Radiance"};
            const ViewFlagsBits values[] = {
                ViewFlagsBits::BaseColor,     ViewFlagsBits::Normal,         ViewFlagsBits::Position,
                ViewFlagsBits::TextureLOD,    ViewFlagsBits::SHARCGrid,      ViewFlagsBits::SHARCOccupancy,
                ViewFlagsBits::SHARCRadiance};
            debugViewChanged = Examples_BitmaskCycleButton(input, "Debug View", cfg.viewFlags, items, values);
        }
        else
        {
            const char* items[] = {"BaseColor", "Normal", "Position", "Texture LOD"};
            const ViewFlagsBits values[] = {ViewFlagsBits::BaseColor, ViewFlagsBits::Normal,
                                            ViewFlagsBits::Position, ViewFlagsBits::TextureLOD};
            debugViewChanged = Examples_BitmaskCycleButton(input, "Debug View", cfg.viewFlags, items, values);
        }
        if (debugViewChanged)
        {
            cfg.viewFlags &= ~kAovViewFlags;
            changed = true;
        }
    }

    {
        const char* items[] = {"White Base Color"};
        const MaterialFlagsBits values[] = {MaterialFlagsBits::DbgWhiteBaseColor};
        changed |= Examples_BitmaskCycleButton(input, "Material Debug", cfg.materialFlags, items, values);
    }

    return changed;
}

ResourceHandle Examples_BuildTonemappingPass(Renderer* renderer, RendererUBO const* globals,
                                             RendererOutputs const& outputs, bool isPresent)
{
    CHECK(globals);
    constexpr RHIResourceFormat kOutputFormat = RHIResourceFormat::R8G8B8A8Unorm;
    // Debug views (e.g. raster overdraw) bypass lighting and write a display-ready image instead of AOVs.
    bool const isDebugOutput = outputs.debugOutput != kInvalidHandle;
    CHECK_MSG(isDebugOutput || outputs.diffuse != kInvalidHandle, "Basic tonemap pass missing diffuse output");
    RHIExtent2D extent = outputs.extent;
    if (extent.x == 0u || extent.y == 0u)
    {
        CHECK_MSG(renderer->IsPresentEnabled(), "Basic tonemap pass requires outputs.extent when running headlessly");
        extent = renderer->GetSwapchainExtent();
    }
    uint32_t const w = extent.x;
    uint32_t const h = extent.y;
    auto linSampler = renderer->CreateSampler({});
    auto postprocessSetup = [=](PassHandle self, Renderer* r)
    {
        if (isDebugOutput)
        {
            r->BindShader(self, RHIShaderStageBits::Fragment, "fragMain",
                          r->GetApplication()->ResolveRelativePathBase("Data/Shaders/PSCopy.spv"));
            r->BindTextureSRV(
                self, outputs.debugOutput, "srcTexture", RHIPipelineStageBits::FragmentShader,
                RHITextureViewDesc{.format = kOutputFormat, .range = RHITextureSubresourceRange::Create()});
            r->BindTextureSampler(self, linSampler, "sampler");
            return;
        }
        r->BindShader(self, RHIShaderStageBits::Fragment, "fragMain",
                      r->GetApplication()->ResolveRelativePathBase("Data/Shaders/PostprocessBasic.spv"));
        r->BindTextureSRV(
            self, outputs.diffuse, "bufferA", RHIPipelineStageBits::FragmentShader,
            RHITextureViewDesc{.format = outputs.aovFormat, .range = RHITextureSubresourceRange::Create()});
        r->BindTextureSRV(
            self, outputs.specular, "bufferB", RHIPipelineStageBits::FragmentShader,
            RHITextureViewDesc{.format = outputs.aovFormat, .range = RHITextureSubresourceRange::Create()});
        r->BindTextureSampler(self, linSampler, "sampler");
        r->BindPushConstant(self, RHIShaderStageBits::Fragment, 0, sizeof(ExamplePostprocessPushConstants));
    };
    auto postprocessRecord = [=](PassHandle self, Renderer* r, RHICommandList* cmd)
    {
        if (isDebugOutput)
            return;
        ExamplePostprocessPushConstants const pc{
            .viewFlags = globals->dbgViewFlags,
            .accumulatedFrames = globals->ptAccumulatedFrames,
        };
        r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Fragment, 0, pc);
    };
    using namespace RenderUtils;
    if (isPresent)
    {
        createPSFullscreenPass(renderer, "Final Blit To Backbuffer", postprocessSetup, postprocessRecord);
        return kInvalidHandle;
    }

    auto postprocess = renderer->CreateResource("Final Image",
                                                RHITextureDesc{.usage = RHITextureUsageBits::RenderTarget |
                                                                   RHITextureUsageBits::SampledImage |
                                                                   RHITextureUsageBits::TransferSource,
                                                               .extent = {w, h, 1},
                                                               .format = kOutputFormat});
    createPSFullscreenPassRTV(
        renderer, "Final Blit To Image", postprocess,
        RHITextureViewDesc{.format = kOutputFormat, .range = RHITextureSubresourceRange::Create()}, {w, h},
        postprocessSetup, postprocessRecord);
    return postprocess;
}

FImportedMesh Examples_MakePlaneMesh(float extent, float y, Allocator* alloc)
{
    FImportedMesh mesh(alloc);
    float const h = extent * 0.5f;
    float3 const n = float3(0, 1, 0);
    float3 const t = float3(1, 0, 0);
    float3 const corners[4] = {
        float3(-h, y, -h),
        float3(h, y, -h),
        float3(-h, y, h),
        float3(h, y, h),
    };
    float2 const uvs[4] = {float2(0, 0), float2(1, 0), float2(0, 1), float2(1, 1)};

    mesh.vertices.resize(4);
    for (uint32_t i = 0; i < 4; ++i)
    {
        FVertex src{};
        src.position = corners[i];
        src.normal = n;
        src.tangent = t;
        src.bitangentSign = 1.0f;
        src.uv = uvs[i];
        mesh.vertices[i] = src;
    }

    mesh.lods.emplace_back(alloc);
    mesh.lods[0].indices.resize(6);
    mesh.lods[0].indices = {0, 2, 1, 1, 2, 3};
    return mesh;
}

FImportedMesh Examples_MakeBoxMesh(float size, Allocator* alloc)
{
    FImportedMesh mesh(alloc);
    float const h = size * 0.5f;

    mesh.vertices.resize(24);
    mesh.lods.emplace_back(alloc);
    mesh.lods[0].indices.resize(36);

    uint32_t v_idx = 0;
    uint32_t i_idx = 0;

    for (int axis = 0; axis < 3; ++axis)
    {
        for (int dir = -1; dir <= 1; dir += 2)
        {
            float3 normal(0, 0, 0);
            normal[axis] = static_cast<float>(dir);

            int u_axis = (axis + 1) % 3;
            int v_axis = (axis + 2) % 3;

            if (dir == -1)
            {
                std::swap(u_axis, v_axis);
            }

            float3 tangent(0, 0, 0);
            tangent[u_axis] = 1.0f;

            float3 bitangent(0, 0, 0);
            bitangent[v_axis] = 1.0f;

            float2 const uvs[4] = {{0, 0}, {1, 0}, {0, 1}, {1, 1}};
            float const u_signs[4] = {-1, 1, -1, 1};
            float const v_signs[4] = {-1, -1, 1, 1};

            uint32_t v_start = v_idx;

            for (int i = 0; i < 4; ++i)
            {
                FVertex src{};
                src.position = normal * h + tangent * (h * u_signs[i]) + bitangent * (h * v_signs[i]);
                src.normal = normal;
                src.tangent = tangent;
                src.bitangentSign = 1.0f;
                src.uv = uvs[i];
                mesh.vertices[v_idx++] = src;
            }

            mesh.lods[0].indices[i_idx++] = v_start + 0;
            mesh.lods[0].indices[i_idx++] = v_start + 1;
            mesh.lods[0].indices[i_idx++] = v_start + 2;
            mesh.lods[0].indices[i_idx++] = v_start + 2;
            mesh.lods[0].indices[i_idx++] = v_start + 1;
            mesh.lods[0].indices[i_idx++] = v_start + 3;
        }
    }
    return mesh;
}

FImportedMesh Examples_MakeSphereMesh(float radius, uint32_t segments, uint32_t rings, Allocator* alloc)
{
    FImportedMesh mesh(alloc);

    uint32_t vertexCount = (rings + 1) * (segments + 1);
    uint32_t indexCount = rings * segments * 6;

    mesh.vertices.resize(vertexCount);
    mesh.lods.emplace_back(alloc);
    mesh.lods[0].indices.resize(indexCount);

    uint32_t v = 0;
    for (uint32_t r = 0; r <= rings; ++r)
    {
        float v_uv = static_cast<float>(r) / static_cast<float>(rings);
        float phi = v_uv * glm::pi<float>();

        for (uint32_t s = 0; s <= segments; ++s)
        {
            float u_uv = static_cast<float>(s) / static_cast<float>(segments);
            float theta = u_uv * 2.0f * glm::pi<float>();

            float x = std::cos(theta) * std::sin(phi);
            float y = std::cos(phi);
            float z = std::sin(theta) * std::sin(phi);

            FVertex& vert = mesh.vertices[v++];
            vert.position = float3(x, y, z) * radius;
            vert.normal = float3(x, y, z);

            float3 tangent(-std::sin(theta), 0.0f, std::cos(theta));
            if (length(tangent) > 0.0001f)
                vert.tangent = normalize(tangent);
            else
                vert.tangent = float3(1, 0, 0);

            vert.bitangentSign = 1.0f;
            vert.uv = float2(u_uv, v_uv);
        }
    }

    uint32_t i = 0;
    for (uint32_t r = 0; r < rings; ++r)
    {
        for (uint32_t s = 0; s < segments; ++s)
        {
            uint32_t v0 = r * (segments + 1) + s;
            uint32_t v1 = v0 + 1;
            uint32_t v2 = v0 + (segments + 1);
            uint32_t v3 = v2 + 1;

            mesh.lods[0].indices[i++] = v0;
            mesh.lods[0].indices[i++] = v2;
            mesh.lods[0].indices[i++] = v1;

            mesh.lods[0].indices[i++] = v1;
            mesh.lods[0].indices[i++] = v2;
            mesh.lods[0].indices[i++] = v3;
        }
    }


    return mesh;
}

#include <Renderer/Rasterizer/GTAO.hpp>
void Example_BuildExampleRasterRenderGraph(Renderer* renderer, RendererUBO* globals, RendererResources& gpu,
                                           RendererConfig& cfg, RendererOutputs& out)
{
    static const GTAOConfig gtaoConfig{};

    RasterFeature features[] = {GTAOFeature(&gtaoConfig)};

    BuildRasterRenderGraph(renderer, globals, gpu, cfg, out, features);
}

void Example_BuildExampleProgressivePathTracerRenderGraph(Renderer* renderer, RendererUBO* globals, RendererResources& gpu,
                                               RendererConfig& cfg, RendererOutputs& out)
{
    BuildProgressivePathTracerRenderGraph(renderer, globals, gpu, cfg, out);
}

void Example_BuildExampleRealtimePathtracerRenderGraph(Renderer* renderer, RendererUBO* globals, RendererResources& gpu,
                                                     RendererConfig& cfg, RendererOutputs& out)
{
    BuildRealtimePathTracerRenderGraph(renderer, globals, gpu, cfg, out);
}

void Example_BuildExampleRenderer(ExampleRenderer renderer, Renderer* r, RendererUBO* globals, RendererResources& gpu,
    RendererConfig& cfg, RendererOutputs& out)
{
    out = RendererOutputs{}; // Handles never outlive their graph; don't leak them into the next build
    switch (renderer)
    {
    case ExampleRenderer::Raster:
        Example_BuildExampleRasterRenderGraph(r, globals, gpu, cfg, out);
        break;
    case ExampleRenderer::RealtimePT:
        Example_BuildExampleRealtimePathtracerRenderGraph(r, globals, gpu, cfg, out);
        break;
    case ExampleRenderer::ProgressivePT:
        Example_BuildExampleProgressivePathTracerRenderGraph(r, globals, gpu, cfg, out);
        break;
    default:
        break;
    }
}