#include "RTScreenReflections.hpp"
#include <RenderUtils/CSMipGeneration.hpp>
#include <algorithm>

namespace
{
constexpr RHIResourceFormat kRadianceFormat = RHIResourceFormat::R16G16B16A16SignedFloat;
constexpr RHIResourceFormat kNormalFormat = RHIResourceFormat::A2B10G10R10Unorm;

struct ReflectionPushConstants
{
    float maxDistance;
    float originBias;
    float relativeDepthTolerance;
    float coneScale;
    float maxRoughness;
    float screenFade;
    float strength;
    uint32_t mipLevels;
};
static_assert(sizeof(ReflectionPushConstants) == 32);

struct ReflectionPrefilterPushConstants
{
    uint32_t width;
    uint32_t height;
    uint32_t mip;
    uint32_t sampleCount;
};
static_assert(sizeof(ReflectionPrefilterPushConstants) == 16);

ResourceHandle BuildReflectionGGXPrefilter(Renderer* renderer, RasterReflectionResources resources,
                                          RHIExtent2D extent, ResourceHandle sampler)
{
    RHIExtent2D baseExtent{(extent.x + 1u) / 2u, (extent.y + 1u) / 2u};
    auto filtered = renderer->CreateResource(
        "Raster Reflection GGX Radiance",
        RHITextureDesc{.usage = RHITextureUsageBits::StorageImage | RHITextureUsageBits::SampledImage,
                       .extent = {baseExtent.x, baseExtent.y, 1},
                       .format = kRadianceFormat,
                       .mipLevels = resources.mipLevels});
    // Each lobe is filtered independently from the unfiltered SPD pyramid, never recursively
    // from another GGX level. Separate storage also avoids overlapping sampled/UAV mip views.
    for (uint32_t mip = 0; mip < resources.mipLevels; ++mip)
    {
        RHIExtent2D mipExtent{std::max(1u, baseExtent.x >> mip), std::max(1u, baseExtent.y >> mip)};
        renderer->CreatePass(
            Format("Reflection Radiance GGX Prefilter {}", mip), RHIDeviceQueueType::Graphics, 0u,
            [=](PassHandle self, Renderer* r)
            {
                r->BindShader(self, RHIShaderStageBits::Compute, "main",
                              r->GetApplication()->ResolveRelativePathBase("Data/Shaders/ECSReflectionPrefilter.spv"));
                r->BindTextureSRV(self, resources.radiance, "sourceRadiance", RHIPipelineStageBits::ComputeShader,
                                  {.format = kRadianceFormat,
                                   .range = RHITextureSubresourceRange::Create(
                                       RHITextureAspectFlagBits::Color, 0, resources.mipLevels)});
                r->BindTextureUAV(self, filtered, "outputRadiance", RHIPipelineStageBits::ComputeShader,
                                  {.format = kRadianceFormat,
                                   .range = RHITextureSubresourceRange::Create(RHITextureAspectFlagBits::Color, mip, 1)});
                r->BindTextureSampler(self, sampler, "radianceSampler");
                r->BindPushConstant(self, RHIShaderStageBits::Compute, 0, sizeof(ReflectionPrefilterPushConstants));
            },
            [=](PassHandle self, Renderer* r, RHICommandList* cmd)
            {
                ReflectionPrefilterPushConstants push{mipExtent.x, mipExtent.y, mip, 64u};
                r->CmdSetPipeline(self, cmd);
                r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Compute, 0, push);
                r->CmdDispatch(self, cmd, {mipExtent.x, mipExtent.y, 1});
            });
    }
    return filtered;
}
} // namespace

RasterReflectionResources CreateRasterReflectionResources(Renderer* renderer, RHIExtent2D extent)
{
    uint32_t halfW = (extent.x + 1u) / 2u;
    uint32_t halfH = (extent.y + 1u) / 2u;
    uint32_t levels = 1u;
    for (uint32_t dimension = std::max(halfW, halfH); dimension > 1u; dimension >>= 1u)
        ++levels;
    return {
        .radiance = renderer->CreateResource(
            "Raster Reflection Radiance",
            RHITextureDesc{.usage = RHITextureUsageBits::StorageImage | RHITextureUsageBits::SampledImage,
                           .extent = {halfW, halfH, 1},
                           .format = kRadianceFormat,
                           .mipLevels = levels}),
        .environmentSpecular = renderer->CreateResource(
            "Raster Environment Specular",
            RHITextureDesc{.usage = RHITextureUsageBits::RenderTarget | RHITextureUsageBits::SampledImage,
                           .extent = {extent.x, extent.y, 1},
                           .format = kRadianceFormat}),
        .mipLevels = levels,
    };
}

void BuildRasterRTScreenReflections(RasterFeatureContext& ctx, RasterReflectionResources resources)
{
    auto* renderer = ctx.renderer;
    CHECK(renderer && ctx.cfg && ctx.gpu && ctx.tlas != kInvalidHandle);
    CHECK(resources.radiance != kInvalidHandle && resources.environmentSpecular != kInvalidHandle);
    auto output = renderer->CreateResource(
        "Raster Reflected Specular",
        RHITextureDesc{.usage = RHITextureUsageBits::StorageImage | RHITextureUsageBits::SampledImage |
                               RHITextureUsageBits::TransferSource | RHITextureUsageBits::RenderTarget,
                       .extent = {ctx.extent.x, ctx.extent.y, 1},
                       .format = kRadianceFormat});
    auto sampler = renderer->CreateSampler({
        .addressMode = {.u = RHIDeviceSampler::SamplerDesc::AddressMode::ClampToEdge,
                        .v = RHIDeviceSampler::SamplerDesc::AddressMode::ClampToEdge,
                        .w = RHIDeviceSampler::SamplerDesc::AddressMode::ClampToEdge},
    });
    // SPD is only the anti-aliasing source pyramid; it is not the specular convolution.
    Foundation::RenderUtils::createCSMipGenerationSinglePass(
        renderer, "Reflection Radiance SPD Mip Gen", RHIDeviceQueueType::Graphics,
        resources.radiance, resources.radiance, kRadianceFormat, kRadianceFormat,
        RHITextureAspectFlagBits::Color, RHITextureAspectFlagBits::Color, sampler, resources.mipLevels);
    auto prefilteredRadiance = BuildReflectionGGXPrefilter(renderer, resources, ctx.extent, sampler);
    auto materialSampler = renderer->CreateSampler(MakeTextureSamplerDesc(*ctx.cfg));
    auto* textures = ctx.gpu->GetTexture2DPool();
    // Capture source handles BEFORE replacing ctx.specular; the source snapshot never contains this effect.
    renderer->CreatePass(
        "Raster RT Screen Reflections", RHIDeviceQueueType::Graphics, 0u,
        [=](PassHandle self, Renderer* r)
        {
            r->BindShader(self, RHIShaderStageBits::Compute, "main",
                          r->GetApplication()->ResolveRelativePathBase("Data/Shaders/ECSRTScreenReflections.spv"));
            r->BindBufferUniform(self, ctx.globalUBO.Current(), RHIPipelineStageBits::ComputeShader, "globalParams");
            r->BindAccelerationStructureSRV(self, ctx.tlas, RHIPipelineStageBits::ComputeShader, "scene");
            r->BindBufferStorageRead(self, ctx.instanceBuffer, RHIPipelineStageBits::ComputeShader, "instances");
            r->BindBufferStorageRead(self, ctx.materialBuffer, RHIPipelineStageBits::ComputeShader, "materials");
            r->BindBufferStorageRead(self, ctx.primitiveBuffer, RHIPipelineStageBits::ComputeShader, "staticPrimitives");
            r->BindBufferStorageRead(self, ctx.dynamicPrimitiveBuffer, RHIPipelineStageBits::ComputeShader, "dynamicPrimitives");
            RHITextureViewDesc color{.format = kRadianceFormat, .range = RHITextureSubresourceRange::Create()};
            r->BindTextureSRV(self, ctx.gbuffer0, "RT0", RHIPipelineStageBits::ComputeShader,
                              {.format = RHIResourceFormat::R8G8B8A8Unorm, .range = RHITextureSubresourceRange::Create()});
            r->BindTextureSRV(self, ctx.gbuffer1.Current(), "RT1", RHIPipelineStageBits::ComputeShader,
                              {.format = kNormalFormat, .range = RHITextureSubresourceRange::Create()});
            r->BindTextureSRV(self, ctx.depth.Current(), "depth", RHIPipelineStageBits::ComputeShader,
                              {.format = RHIResourceFormat::D32SignedFloat,
                               .range = RHITextureSubresourceRange::Create(RHITextureAspectFlagBits::Depth)});
            r->BindTextureSRV(self, ctx.instanceID, "instanceID", RHIPipelineStageBits::ComputeShader,
                              {.format = RHIResourceFormat::R32Uint, .range = RHITextureSubresourceRange::Create()});
            r->BindTextureSRV(self, ctx.diffuse, "sourceDiffuse", RHIPipelineStageBits::ComputeShader, color);
            r->BindTextureSRV(self, ctx.specular, "sourceSpecular", RHIPipelineStageBits::ComputeShader, color);
            r->BindTextureSRV(self, resources.environmentSpecular, "environmentSpecular", RHIPipelineStageBits::ComputeShader, color);
            r->BindTextureSRV(self, prefilteredRadiance, "sceneRadiance", RHIPipelineStageBits::ComputeShader,
                              {.format = kRadianceFormat,
                               .range = RHITextureSubresourceRange::Create(RHITextureAspectFlagBits::Color, 0, resources.mipLevels)});
            r->BindTextureUAV(self, output, "outputSpecular", RHIPipelineStageBits::ComputeShader, color);
            r->BindTextureSampler(self, sampler, "radianceSampler");
            r->BindTextureSampler(self, sampler, "gLutSampler");
            r->BindTextureSampler(self, materialSampler, "materialSampler");
            r->BindDescriptorSetRead(self, "gTextures2D", textures->GetDescriptorSetLayout());
            r->BindPushConstant(self, RHIShaderStageBits::Compute, 0, sizeof(ReflectionPushConstants));
        },
        [=](PassHandle self, Renderer* r, RHICommandList* cmd)
        {
            auto const& cfg = *ctx.cfg;
            ReflectionPushConstants push{
                .maxDistance = std::max(cfg.rasterReflectionMaxDistance, 1e-3f),
                .originBias = std::max(cfg.rasterReflectionOriginBias, 0.0f),
                .relativeDepthTolerance = std::clamp(cfg.rasterReflectionDepthTolerance, 0.0f, 0.1f),
                .coneScale = std::clamp(cfg.rasterReflectionConeScale, 0.0f, 4.0f),
                .maxRoughness = std::clamp(cfg.rasterReflectionMaxRoughness, 0.01f, 1.0f),
                .screenFade = std::clamp(cfg.rasterReflectionScreenFade, 0.0f, 0.5f),
                .strength = std::clamp(cfg.rasterReflectionStrength, 0.0f, 1.0f),
                .mipLevels = resources.mipLevels,
            };
            r->CmdSetPipeline(self, cmd);
            r->CmdBindDescriptorSet(self, cmd, "gTextures2D", textures->GetDescriptorSet());
            r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Compute, 0, push);
            r->CmdDispatch(self, cmd, {ctx.extent.x, ctx.extent.y, 1});
        });
    ctx.specular = output;
}
