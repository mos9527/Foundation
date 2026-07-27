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
    if (Examples_Button(input, Format("[{}]", currentRenderer)))
    {
        currentRenderer = static_cast<ExampleRenderer>((static_cast<uint32_t>(currentRenderer) + 1) %
                                                       static_cast<uint32_t>(ExampleRenderer::Count));
        changed = true;
    }
    return changed;
}

ResourceHandle Examples_BuildTonemappingPass(Renderer* renderer, RendererOutputs const& outputs, bool isPresent)
{
    CHECK_MSG(outputs.diffuse != kInvalidHandle, "Basic tonemap pass missing diffuse output");
    RHIExtent2D extent = outputs.extent;
    if (extent.x == 0u || extent.y == 0u)
    {
        CHECK_MSG(renderer->IsPresentEnabled(), "Basic tonemap pass requires outputs.extent when running headlessly");
        extent = renderer->GetSwapchainExtent();
    }
    uint32_t const w = extent.x;
    uint32_t const h = extent.y;
    constexpr RHIResourceFormat kOutputFormat = RHIResourceFormat::R8G8B8A8Unorm;
    auto linSampler = renderer->CreateSampler({});
    auto postprocessSetup = [=](PassHandle self, Renderer* r)
    {
        r->BindShader(self, RHIShaderStageBits::Fragment, "fragMain",
                      r->GetApplication()->ResolveRelativePathBase("Data/Shaders/PostprocessBasic.spv"));
        r->BindTextureSRV(
            self, outputs.diffuse, "bufferA", RHIPipelineStageBits::FragmentShader,
            RHITextureViewDesc{.format = outputs.aovFormat, .range = RHITextureSubresourceRange::Create()});
        r->BindTextureSRV(
            self, outputs.specular, "bufferB", RHIPipelineStageBits::FragmentShader,
            RHITextureViewDesc{.format = outputs.aovFormat, .range = RHITextureSubresourceRange::Create()});
        r->BindTextureSampler(self, linSampler, "sampler");
    };
    using namespace RenderUtils;
    if (isPresent)
    {
        createPSFullscreenPass(renderer, "Final Blit To Backbuffer", postprocessSetup);
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
        RHITextureViewDesc{.format = kOutputFormat, .range = RHITextureSubresourceRange::Create()}, postprocessSetup);
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