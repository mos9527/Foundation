#include <Core/Paths.hpp>
#include <algorithm>
#include "RasterEffects.hpp"
using namespace Foundation;
using namespace Foundation::Core;
using namespace Foundation::RenderCore;

namespace
{
constexpr RHIResourceFormat kGBufferNormalFormat = RHIResourceFormat::A2B10G10R10Unorm;
constexpr RHIResourceFormat kAOFormat = RHIResourceFormat::R16Unorm;

void BuildRasterGTAO(RasterEffectContext& ctx, void const* configPtr)
{
    CHECK(configPtr);
    auto* config = static_cast<RasterGTAOConfig const*>(configPtr);
    auto* renderer = ctx.renderer;
    uint32_t w = ctx.extent.x;
    uint32_t h = ctx.extent.y;
    uint32_t halfW = std::max((w + 1u) / 2u, 1u);
    uint32_t halfH = std::max((h + 1u) / 2u, 1u);
    auto HalfAO = renderer->CreateResource(
        "GTAO Half",
        RHITextureDesc{.usage = RHITextureUsageBits::StorageImage | RHITextureUsageBits::SampledImage,
                       .extent = {halfW, halfH, 1},
                       .format = kAOFormat});
    auto AO = renderer->CreateResource(
        "GTAO",
        RHITextureDesc{.usage = RHITextureUsageBits::StorageImage | RHITextureUsageBits::SampledImage,
                       .extent = {w, h, 1},
                       .format = kAOFormat});

    renderer->CreatePass(
        "GTAO", RHIDeviceQueueType::Compute, 0u,
        [=](PassHandle self, Renderer* r)
        {
            r->BindShader(self, RHIShaderStageBits::Compute, "main", PathsResolve("Data/Shaders/ECSGTAO.spv"));
            r->BindBufferUniform(self, ctx.globalUBO, RHIPipelineStageBits::ComputeShader, "globalParams");
            r->BindTextureSRV(self, ctx.gbuffer1, "RT1", RHIPipelineStageBits::ComputeShader,
                              RHITextureViewDesc{.format = kGBufferNormalFormat,
                                                 .range = RHITextureSubresourceRange::Create()});
            r->BindTextureSRV(
                self, ctx.depth, "depth", RHIPipelineStageBits::ComputeShader,
                RHITextureViewDesc{.format = RHIResourceFormat::D32SignedFloat,
                                   .range = RHITextureSubresourceRange::Create(RHITextureAspectFlagBits::Depth)});
            r->BindTextureUAV(self, HalfAO, "aoOutput", RHIPipelineStageBits::ComputeShader,
                              RHITextureViewDesc{.format = kAOFormat,
                                                 .range = RHITextureSubresourceRange::Create()});
            r->BindPushConstant(self, RHIShaderStageBits::Compute, 0, sizeof(RasterGTAOConfig));
        },
        [=](PassHandle self, Renderer* r, RHICommandList* cmd)
        {
            r->CmdSetPipeline(self, cmd);
            r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Compute, 0, *config);
            r->CmdDispatch(self, cmd, {halfW, halfH, 1});
        });

    renderer->CreatePass(
        "GTAO Upsample", RHIDeviceQueueType::Compute, 0u,
        [=](PassHandle self, Renderer* r)
        {
            r->BindShader(self, RHIShaderStageBits::Compute, "main", PathsResolve("Data/Shaders/ECSGTAOUpsample.spv"));
            r->BindBufferUniform(self, ctx.globalUBO, RHIPipelineStageBits::ComputeShader, "globalParams");
            r->BindTextureSRV(self, HalfAO, "halfAO", RHIPipelineStageBits::ComputeShader,
                              RHITextureViewDesc{.format = kAOFormat,
                                                 .range = RHITextureSubresourceRange::Create()});
            r->BindTextureSRV(
                self, ctx.depth, "depth", RHIPipelineStageBits::ComputeShader,
                RHITextureViewDesc{.format = RHIResourceFormat::D32SignedFloat,
                                   .range = RHITextureSubresourceRange::Create(RHITextureAspectFlagBits::Depth)});
            r->BindTextureUAV(self, AO, "aoOutput", RHIPipelineStageBits::ComputeShader,
                              RHITextureViewDesc{.format = kAOFormat,
                                                 .range = RHITextureSubresourceRange::Create()});
        },
        [=](PassHandle self, Renderer* r, RHICommandList* cmd)
        {
            r->CmdSetPipeline(self, cmd);
            r->CmdDispatch(self, cmd, {w, h, 1});
        });

    ctx.ambientOcclusion = AO;
}

void BuildRasterMotionBlur(RasterEffectContext& ctx, void const* configPtr)
{
    CHECK(configPtr);
    CHECK(ctx.motionVectors != kInvalidHandle);
    CHECK(ctx.diffuse != kInvalidHandle);
    CHECK(ctx.specular != kInvalidHandle);
    CHECK(ctx.depth != kInvalidHandle);
    auto* config = static_cast<RasterMotionBlurConfig const*>(configPtr);
    auto* renderer = ctx.renderer;
    uint32_t w = ctx.extent.x;
    uint32_t h = ctx.extent.y;
    constexpr RHIResourceFormat kAOVFormat = RHIResourceFormat::R16G16B16A16SignedFloat;
    auto BlurredDiffuse = renderer->CreateResource(
        "Motion Blur Diffuse",
        RHITextureDesc{.usage = RHITextureUsageBits::StorageImage | RHITextureUsageBits::SampledImage |
                             RHITextureUsageBits::TransferSource,
                       .extent = {w, h, 1},
                       .format = kAOVFormat});
    auto BlurredSpecular = renderer->CreateResource(
        "Motion Blur Specular",
        RHITextureDesc{.usage = RHITextureUsageBits::StorageImage | RHITextureUsageBits::SampledImage |
                             RHITextureUsageBits::TransferSource,
                       .extent = {w, h, 1},
                       .format = kAOVFormat});
    auto ColorSampler = renderer->CreateSampler({
        .addressMode = {.u = RHIDeviceSampler::SamplerDesc::AddressMode::ClampToEdge,
                        .v = RHIDeviceSampler::SamplerDesc::AddressMode::ClampToEdge,
                        .w = RHIDeviceSampler::SamplerDesc::AddressMode::ClampToEdge},
    });

    auto const diffuseIn = ctx.diffuse;
    auto const specularIn = ctx.specular;
    auto const motionVectors = ctx.motionVectors;
    auto const depth = ctx.depth;
    auto const globalUBO = ctx.globalUBO;

    renderer->CreatePass(
        "Motion Blur", RHIDeviceQueueType::Graphics, 0u,
        [=](PassHandle self, Renderer* r)
        {
            r->BindShader(self, RHIShaderStageBits::Compute, "main",
                          PathsResolve("Data/Shaders/ECSMotionBlur.spv"));
            r->BindBufferUniform(self, globalUBO, RHIPipelineStageBits::ComputeShader, "globalParams");
            r->BindTextureSampler(self, ColorSampler, "colorSampler");
            r->BindTextureSRV(self, diffuseIn, "diffuseInput", RHIPipelineStageBits::ComputeShader,
                              RHITextureViewDesc{.format = kAOVFormat,
                                                 .range = RHITextureSubresourceRange::Create()});
            r->BindTextureSRV(self, specularIn, "specularInput", RHIPipelineStageBits::ComputeShader,
                              RHITextureViewDesc{.format = kAOVFormat,
                                                 .range = RHITextureSubresourceRange::Create()});
            r->BindTextureSRV(self, motionVectors, "motionVectors", RHIPipelineStageBits::ComputeShader,
                              RHITextureViewDesc{.format = RHIResourceFormat::R16G16SignedFloat,
                                                 .range = RHITextureSubresourceRange::Create()});
            r->BindTextureSRV(
                self, depth, "depth", RHIPipelineStageBits::ComputeShader,
                RHITextureViewDesc{.format = RHIResourceFormat::D32SignedFloat,
                                   .range = RHITextureSubresourceRange::Create(RHITextureAspectFlagBits::Depth)});
            r->BindTextureUAV(self, BlurredDiffuse, "diffuseOutput", RHIPipelineStageBits::ComputeShader,
                              RHITextureViewDesc{.format = kAOVFormat,
                                                 .range = RHITextureSubresourceRange::Create()});
            r->BindTextureUAV(self, BlurredSpecular, "specularOutput", RHIPipelineStageBits::ComputeShader,
                              RHITextureViewDesc{.format = kAOVFormat,
                                                 .range = RHITextureSubresourceRange::Create()});
            r->BindPushConstant(self, RHIShaderStageBits::Compute, 0, sizeof(RasterMotionBlurConfig));
        },
        [=](PassHandle self, Renderer* r, RHICommandList* cmd)
        {
            r->CmdSetPipeline(self, cmd);
            r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Compute, 0, *config);
            r->CmdDispatch(self, cmd, {w, h, 1});
        });

    ctx.diffuse = BlurredDiffuse;
    ctx.specular = BlurredSpecular;
}
} // namespace

RasterEffect MakeRasterGTAOEffect(RasterGTAOConfig const* config)
{
    return RasterEffect{
        .injectionPoint = RasterInjectionPoint::BeforeLighting,
        .order = 0,
        .callback = BuildRasterGTAO,
        .config = config,
    };
}

RasterEffect MakeRasterMotionBlurEffect(RasterMotionBlurConfig const* config)
{
    return RasterEffect{
        .injectionPoint = RasterInjectionPoint::AfterLighting,
        .order = 0,
        .callback = BuildRasterMotionBlur,
        .config = config,
    };
}
