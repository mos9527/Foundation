#include <RenderUtils/CSClearBuffer.hpp>
#include <RenderUtils/CSMipGeneration.hpp>
#include <RenderUtils/PSFullscreen.hpp>
#include <algorithm>
#include "GPUScene.hpp"
#include "ProgressivePathtracer.hpp"

uint32_t PackCompileOptions(PTSampler sampler, bool forceTextureLOD0, LightSampler lightSamplerMode)
{
    uint32_t options = 0u;
    options |= sampler == PTSampler::PCG;
    options |= forceTextureLOD0 ? to_integer(PTCompileOptionsBits::ForceTextureLOD0) : 0u;
    options |= lightSamplerMode == LightSampler::Uniform ? to_integer(PTCompileOptionsBits::LightSamplerUniform) : 0u;
    return options;
}

void BuildRealtimePathTracerRenderGraph(Renderer* renderer, RendererUBO* globals, RendererResources& gpu,
                                RendererConfig const& cfg, RendererOutputs& out)
{
    CHECK(renderer);
    CHECK(globals);
    CHECK(gpu.scene);
    CHECK_MSG(renderer->GetDevice()->GetCapabilities().raytracingInline, "Pathtracer requires Ray Query support");
    out = {};
    globals->ptAccumulatedFrames = 0u;
    RHIExtent2D renderExtent = cfg.renderExtent;
    if (renderExtent.x == 0u || renderExtent.y == 0u)
        renderExtent = renderer->GetSwapchainExtent();
    globals->fbWidth = static_cast<float>(renderExtent.x);
    globals->fbHeight = static_cast<float>(renderExtent.y);
    auto GlobalUBO = renderer->CreateResource(
        "Global UBO",
        RHIBufferDesc{.usage = RHIBufferUsageBits::TransferDestination | RHIBufferUsageBits::UniformBuffer,
                      .size = sizeof(RendererUBO)});
    renderer->CreatePass(
        "UBO Update", RHIDeviceQueueType::Graphics, 0u,
        [=](PassHandle self, Renderer* r) { r->BindBufferCopyDst(self, GlobalUBO); },
        [=](PassHandle, Renderer* r, RHICommandList* cmd)
        {
            auto* ubo = r->DerefResource(GlobalUBO).Get<RHIBuffer*>();
            cmd->UpdateBuffer(ubo, 0, AsBytes(AsSpan(*globals)));
        });
    CHECK(gpu.tlas != kInvalidHandle && "Pathtracer requires a TLAS to be built/updated from.");
    BuildGPUSceneAccelerationStructureUpdatePass(renderer, gpu);
    if (cfg.lightSamplerMode == LightSampler::BVH)
        BuildGPUSceneLightBVHRefitPasses(renderer, gpu, GlobalUBO);
    ResourceHandle TLAS = gpu.tlas;
    /* Instance and Primitive buffers */
    auto PrimitiveBuffer = gpu.primitiveBuffer;
    auto DynamicPrimitiveBuffer = gpu.dynamicPrimitiveBuffer;
    auto InstanceBuffer = gpu.instanceBuffer;
    auto MaterialBuffer = gpu.materialBuffer;
    auto LightBuffer = gpu.lightBuffer;
    auto LightBVHNodeBuffer = gpu.lightBVHNodeBuffer;
    auto LightBVHLightIndexBuffer = gpu.lightBVHLightIndexBuffer;
    auto LightBVHBitmaskBuffer = gpu.lightBVHBitmaskBuffer;
    auto LightBVHNodeIndexBuffer = gpu.lightBVHNodeIndexBuffer;
    auto TexSampler = renderer->CreateSampler(MakeTextureSamplerDesc(cfg));
    uint32_t w = std::max(renderExtent.x, 1u);
    uint32_t h = std::max(renderExtent.y, 1u);
    constexpr RHIResourceFormat kPathTracerAOVFormat = RHIResourceFormat::R16G16B16A16SignedFloat;
    // AOV buffers
    auto Diffuse = renderer->CreateResource("Diffuse",
                                            RHITextureDesc{.usage = RHITextureUsageBits::StorageImage |
                                                               RHITextureUsageBits::SampledImage |
                                                               RHITextureUsageBits::TransferSource,
                                                           .extent = {w, h, 1},
                                                           .format = kPathTracerAOVFormat});
    auto Specular = renderer->CreateResource("Specular",
                                             RHITextureDesc{.usage = RHITextureUsageBits::StorageImage |
                                                                RHITextureUsageBits::SampledImage |
                                                                RHITextureUsageBits::TransferSource,
                                                            .extent = {w, h, 1},
                                                            .format = kPathTracerAOVFormat});
    // PT writes depth as a color UAV; PS-blit to a D32 DSV so consumers match raster.
    auto DepthUAV = renderer->CreateResource(
        "Depth UAV",
        RHITextureDesc{.usage = RHITextureUsageBits::StorageImage | RHITextureUsageBits::SampledImage,
                       .extent = {w, h, 1},
                       .format = RHIResourceFormat::R32SignedFloat});
    auto Depth = renderer->CreateResource(
        "Depth",
        RHITextureDesc{.usage = RHITextureUsageBits::DepthStencil | RHITextureUsageBits::SampledImage,
                       .extent = {w, h, 1},
                       .format = RHIResourceFormat::D32SignedFloat});
    // Instance ID map: R32_UINT, written every frame on primary hit (no accumulation)
    auto InstanceIDBuffer = renderer->CreateResource(
        "Instance ID Buffer",
        RHITextureDesc{.usage = RHITextureUsageBits::StorageImage | RHITextureUsageBits::SampledImage,
                       .extent = {w, h, 1},
                       .format = RHIResourceFormat::R32Uint});
    ResourceHandle EnvMapSampler = renderer->CreateSampler({
        .filter = {RHIDeviceSampler::SamplerDesc::Filter::Linear, RHIDeviceSampler::SamplerDesc::Filter::Linear},
    });
    auto LUTSampler = renderer->CreateSampler({.addressMode = {
                                                   .u = RHIDeviceSampler::SamplerDesc::AddressMode::ClampToEdge,
                                                   .v = RHIDeviceSampler::SamplerDesc::AddressMode::ClampToEdge,
                                                   .w = RHIDeviceSampler::SamplerDesc::AddressMode::ClampToEdge,
                                               }});
    const bool shaderExecutionReordering =
        cfg.ptShaderExecutionReordering && renderer->GetDevice()->GetCapabilities().shaderExecutionReordering;
    RenderUtils::createCSClearTexture(
        renderer, "Trace Clear Depth", DepthUAV,
        RHITextureViewDesc{.format = RHIResourceFormat::R32SignedFloat, .range = RHITextureSubresourceRange::Create()},
        float4{0.0f, 0.0f, 0.0f, 0.0f});
    String tracePassName = Format("Trace {}", shaderExecutionReordering ? "SER" : "Compute");
    renderer->CreatePass(
        tracePassName.c_str(), RHIDeviceQueueType::Graphics, 0u,
        [=](PassHandle self, Renderer* r)
        {
            const auto pipelineStage = shaderExecutionReordering ? RHIPipelineStageBits::RayTracingShader
                                                                 : RHIPipelineStageBits::ComputeShader;
            r->BindBufferUniform(self, GlobalUBO, pipelineStage, "globalParams");
            r->BindAccelerationStructureSRV(self, TLAS, pipelineStage, "TLAS");
            const uint kCompileOptions =
                PackCompileOptions(cfg.ptSampler, cfg.forceTextureLOD0, cfg.lightSamplerMode);
            const auto shader = r->GetApplication()->ResolveRelativePathBase(!shaderExecutionReordering ? "Data/Shaders/EPathTracingRealtime.spv"
                                                                        : "Data/Shaders/EPathTracingRealtime_SER.spv");
            if (shaderExecutionReordering)
            {
                r->BindShader(self, RHIShaderStageBits::RayGeneration, "RayGeneration", shader,
                              AsBytes(AsSpan(kCompileOptions)));
            }
            else
            {
                r->BindShader(self, RHIShaderStageBits::Compute, "ComputeMain", shader,
                              AsBytes(AsSpan(kCompileOptions)));
            }

            r->BindBufferStorageRead(self, PrimitiveBuffer, pipelineStage, "gPrimBuffer");
            r->BindBufferStorageRead(self, DynamicPrimitiveBuffer, pipelineStage, "gDynamicPrimBuffer");
            r->BindBufferStorageRead(self, InstanceBuffer, pipelineStage, "gInstances");
            r->BindBufferStorageRead(self, MaterialBuffer, pipelineStage, "gMaterials");
            r->BindBufferStorageRead(self, LightBuffer, pipelineStage, "gLights");
            // Sampler
            r->BindTextureSampler(self, TexSampler, "gTexSampler");
            // Light BVH
            r->BindBufferStorageRead(self, LightBVHNodeBuffer, pipelineStage, "lightBVHNodes");
            r->BindBufferStorageRead(self, LightBVHLightIndexBuffer, pipelineStage, "lightBVHLightIndices");
            r->BindBufferStorageRead(self, LightBVHBitmaskBuffer, pipelineStage, "lightBVHBitmasks");
            r->BindTextureSampler(self, LUTSampler, "gLutSampler");
            // Accumulation UAVs
            r->BindTextureUAV(
                self, Diffuse, "diffuse", pipelineStage,
                RHITextureViewDesc{.format = kPathTracerAOVFormat, .range = RHITextureSubresourceRange::Create()});
            r->BindTextureUAV(
                self, Specular, "specular", pipelineStage,
                RHITextureViewDesc{.format = kPathTracerAOVFormat, .range = RHITextureSubresourceRange::Create()});
            r->BindTextureUAV(self, DepthUAV, "depth", pipelineStage,
                              RHITextureViewDesc{.format = RHIResourceFormat::R32SignedFloat,
                                                 .range = RHITextureSubresourceRange::Create()});
            r->BindTextureUAV(self, InstanceIDBuffer, "instanceIDBuffer", pipelineStage,
                              RHITextureViewDesc{.format = RHIResourceFormat::R32Uint,
                                                 .range = RHITextureSubresourceRange::Create()});
            r->BindTextureSampler(self, EnvMapSampler, "gEnvMapSampler");
            r->BindDescriptorSetRead(self, "gTextures2D", gpu.textures2D->GetDescriptorSetLayout());
        },
        [=](PassHandle self, Renderer* r, RHICommandList* cmd)
        {
            r->CmdSetPipeline(self, cmd);
            r->CmdBindDescriptorSet(self, cmd, "gTextures2D", gpu.textures2D->GetDescriptorSet());
            bool canTrace = (!cfg.ptRenderPaused || !*cfg.ptRenderPaused);
            if (canTrace)
            {
                if (shaderExecutionReordering)
                    cmd->TraceRays(w, h, 1);
                else
                    cmd->Dispatch((w - 1) / 8 + 1, (h - 1) / 8 + 1, 1);
            }
        });
    RenderUtils::createPSDepthCopyPass(renderer, "Copy Depth UAV to DSV", DepthUAV, Depth, {w, h});
    out.extent = {w, h};
    out.aovFormat = kPathTracerAOVFormat;
    out.diffuse = Diffuse;
    out.specular = Specular;
    out.depth = Depth;
    out.instanceID = InstanceIDBuffer;
}
