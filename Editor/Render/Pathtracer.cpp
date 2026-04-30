#include <RenderUtils/CSClearBuffer.hpp>
#include <RenderUtils/CSMipGeneration.hpp>
#include <RenderUtils/PSFullscreen.hpp>
#include <algorithm>
#include "../Paths.hpp"
#include "Render.hpp"
using namespace RenderUtils;

void BuildPathTracerRenderGraph(FContext* context, RendererConfig cfg, RendererScene scene, RHIExtent2D renderExtent,
                                RendererHandles& outHandles, bool const* renderPaused)
{
    CHECK(context->device->GetCapabilities().raytracingPipeline);
    auto* renderer = context->renderer;
    CHECK(renderer);
    auto* gpu = context->gpuScene;
    scene.gsGlobals->ptAccumulatedFrames = 0u;
    auto GlobalUBO = renderer->CreateResource(
        "Global UBO",
        RHIBufferDesc{.usage = RHIBufferUsageBits::TransferDestination | RHIBufferUsageBits::UniformBuffer,
                      .size = sizeof(UBO)});
    renderer->CreatePass(
        "UBO Update", RHIDeviceQueueType::Graphics, 0u,
        [=](PassHandle self, Renderer* r) { r->BindBufferCopyDst(self, GlobalUBO); },
        [=](PassHandle, Renderer* r, RHICommandList* cmd)
        {
            auto* ubo = r->DerefResource(GlobalUBO).Get<RHIBuffer*>();
            cmd->UpdateBuffer(ubo, 0, AsBytes(AsSpan(*scene.gsGlobals)));
        });
    auto TLAS = renderer->CreateResource("Scene TLAS", gpu->GetTLAS());
    renderer->CreatePass(
        "TLAS Update", RHIDeviceQueueType::Graphics, 0u, [=](PassHandle self, Renderer* r)
        { r->BindAccelerationStructureWrite(self, TLAS); }, [=](PassHandle, Renderer* r, RHICommandList* cmd)
        {
            gpu->BuildTLAS(cmd, *scene.gsInstances, *scene.gsBLASes, *scene.gsLights, true);
        });
    /* Instance and Primitive buffers */
    auto PrimitiveBuffer = renderer->CreateResource("Primitive Buffer", gpu->GetPrimitiveBuffer());
    auto InstanceBuffer = renderer->CreateResource("Instance Buffer", gpu->GetInstanceBuffer());
    auto MaterialBuffer = renderer->CreateResource("Material Buffer", gpu->GetMaterialBuffer());
    auto LightBuffer = renderer->CreateResource("Light Buffer", gpu->GetLightBuffer());
    auto LightAliasTableBuffer = renderer->CreateResource("Light Alias Table Buffer", gpu->GetLightAliasTableBuffer());
    auto SobolMatricesBuffer = renderer->CreateResource("Sobol Matrices Buffer", gpu->GetSobolMatricesBuffer());
    
    auto GGXlutE = renderer->CreateResource("GGX LUT E", gpu->GetGGXlutE());
    auto GGXlutEavg = renderer->CreateResource("GGX LUT Eavg", gpu->GetGGXlutEavg());
    ResourceHandle EnvMapTex;
    if (gpu->GetEnvMap()) {
        EnvMapTex = renderer->CreateResource("Env Map", gpu->GetEnvMap());
    } else {
        EnvMapTex = renderer->CreateResource("Env Map Fallback", RHITextureDesc{
            .usage = RHITextureUsageBits::SampledImage | RHITextureUsageBits::TransferDestination,
            .extent = {1, 1, 1},
            .format = RHIResourceFormat::R8G8B8A8Unorm});
    }
    auto TexSampler = renderer->CreateSampler({});
    uint32_t w = std::max(renderExtent.x, 1u);
    uint32_t h = std::max(renderExtent.y, 1u);

    // -- Accumulation buffers (Welford online mean, all F32)
    auto Diffuse = renderer->CreateResource("Diffuse",
                                            RHITextureDesc{.usage = RHITextureUsageBits::StorageImage |
                                                               RHITextureUsageBits::SampledImage |
                                                               RHITextureUsageBits::TransferSource,
                                                           .extent = {w, h, 1},
                                                           .format = RHIResourceFormat::R32G32B32A32SignedFloat});
    auto Specular = renderer->CreateResource("Specular",
                                             RHITextureDesc{.usage = RHITextureUsageBits::StorageImage |
                                                                RHITextureUsageBits::SampledImage |
                                                                RHITextureUsageBits::TransferSource,
                                                            .extent = {w, h, 1},
                                                            .format = RHIResourceFormat::R32G32B32A32SignedFloat});
    auto GBuffer0 = renderer->CreateResource(
        "GBuffer 0",
        RHITextureDesc{.usage = RHITextureUsageBits::StorageImage | RHITextureUsageBits::SampledImage,
                       .extent = {w, h, 1},
                       .format = RHIResourceFormat::R32G32B32A32SignedFloat});
    auto GBuffer1 = renderer->CreateResource(
        "GBuffer 1",
        RHITextureDesc{.usage = RHITextureUsageBits::StorageImage | RHITextureUsageBits::SampledImage,
                       .extent = {w, h, 1},
                       .format = RHIResourceFormat::R32G32B32A32SignedFloat});
    auto GBuffer2 = renderer->CreateResource(
        "GBuffer 2",
        RHITextureDesc{.usage = RHITextureUsageBits::StorageImage | RHITextureUsageBits::SampledImage,
                       .extent = {w, h, 1},
                       .format = RHIResourceFormat::R32G32B32A32SignedFloat});
    // Instance ID map: R32_UINT, written every frame on primary hit (no accumulation)
    auto PickIDBuffer = renderer->CreateResource(
        "Pick ID Buffer",
        RHITextureDesc{.usage = RHITextureUsageBits::StorageImage | RHITextureUsageBits::SampledImage,
                       .extent = {w, h, 1},
                       .format = RHIResourceFormat::R32Uint});
    // 4-byte persistently-mapped readback buffer: Blit PS writes the picked instanceID here.
    auto PickResultBuffer =
        renderer->CreateResource("Pick Result Buffer",
                                 RHIBufferDesc{.resource = {.heap = RHIDeviceHeapType::Readback,
                                                            .hostAccess = RHIResourceHostAccess::ReadWrite,
                                                            .coherent = true},
                                               .usage = RHIBufferUsageBits::StorageBuffer,
                                               .size = sizeof(uint32_t)});
    // Save handles for editor HDR export
    outHandles.hdrRT[0] = Diffuse;
    outHandles.hdrRT[1] = Specular;
    outHandles.numHdrRT = 2u;
    outHandles.pickBuffer = PickResultBuffer;

    ResourceHandle EnvMapMarginalCDF;
    ResourceHandle EnvMapConditionalCDF;
    if (gpu->GetEnvMap())
    {
        EnvMapMarginalCDF = renderer->CreateResource("Env Map Marginal CDF", gpu->GetEnvMapMarginalCDF());
        EnvMapConditionalCDF = renderer->CreateResource("Env Map Conditional CDF", gpu->GetEnvMapConditionalCDF());
    }
    else
    {
        EnvMapMarginalCDF = renderer->CreateResource(
            "Env Map Marginal CDF Fallback",
            RHIBufferDesc{.usage = RHIBufferUsageBits::StorageBuffer, .size = sizeof(float) * 2});
        EnvMapConditionalCDF = renderer->CreateResource(
            "Env Map Conditional CDF Fallback",
            RHITextureDesc{.usage = RHITextureUsageBits::SampledImage | RHITextureUsageBits::TransferDestination,
                           .extent = {1, 1, 1},
                           .format = RHIResourceFormat::R32SignedFloat});
    }
    auto EnvMapSampler = renderer->CreateSampler({
        .filter = {RHIDeviceSampler::SamplerDesc::Filter::Linear, RHIDeviceSampler::SamplerDesc::Filter::Linear},
    });
    auto LUTSampler = renderer->CreateSampler({.addressMode = {
                                                   .u = RHIDeviceSampler::SamplerDesc::AddressMode::ClampToEdge,
                                                   .v = RHIDeviceSampler::SamplerDesc::AddressMode::ClampToEdge,
                                                   .w = RHIDeviceSampler::SamplerDesc::AddressMode::ClampToEdge,
                                               }});
    renderer->CreatePass(
        "Trace", RHIDeviceQueueType::Graphics, 0u,
        [=](PassHandle self, Renderer* r)
        {
            r->BindBufferUniform(self, GlobalUBO, RHIPipelineStageBits::RayTracingShader, "globalParams");
            r->BindAccelerationStructureSRV(self, TLAS, RHIPipelineStageBits::RayTracingShader, "AS");
            const uint capabilityFlags = context->device->GetCapabilities().shaderExecutionReordering;
            r->BindShader(self, RHIShaderStageBits::RayGeneration, "RayGeneration",
                          Paths::Resolve("data/shaders/ERTPathTracer.spv"), AsBytes(AsSpan(capabilityFlags)));
            r->BindShader(self, RHIShaderStageBits::RayClosestHit, "RayClosestHit",
                          Paths::Resolve("data/shaders/ERTPathTracer.spv"), AsBytes(AsSpan(capabilityFlags)),
                          /*hit group*/ 0);
            r->BindShader(self, RHIShaderStageBits::RayAnyHit, "RayOpacityAnyHit",
                          Paths::Resolve("data/shaders/ERTPathTracer.spv"), AsBytes(AsSpan(capabilityFlags)),
                          /*hit group*/ 0);
            r->BindShader(self, RHIShaderStageBits::RayMiss, "RayMiss",
                          Paths::Resolve("data/shaders/ERTPathTracer.spv"), AsBytes(AsSpan(capabilityFlags)));
            r->BindShader(self, RHIShaderStageBits::RayAnyHit, "ShadowRayAnyHit",
                          Paths::Resolve("data/shaders/ERTPathTracer.spv"), AsBytes(AsSpan(capabilityFlags)),
                          /*hit group*/ 1);
            r->BindShader(self, RHIShaderStageBits::RayMiss, "ShadowRayMiss",
                          Paths::Resolve("data/shaders/ERTPathTracer.spv"), AsBytes(AsSpan(capabilityFlags)));
            r->BindShader(self, RHIShaderStageBits::RayAnyHit, "BSSRDFQueryAnyHit",
                          Paths::Resolve("data/shaders/ERTPathTracer.spv"), AsBytes(AsSpan(capabilityFlags)),
                          /*hit group*/ 2);
            r->BindShader(self, RHIShaderStageBits::RayMiss, "BSSRDFQueryMiss",
                          Paths::Resolve("data/shaders/ERTPathTracer.spv"), AsBytes(AsSpan(capabilityFlags)));
            r->BindBufferStorageRead(self, PrimitiveBuffer, RHIPipelineStageBits::ComputeShader, "primitives");
            r->BindBufferStorageRead(self, InstanceBuffer, RHIPipelineStageBits::ComputeShader, "instances");
            r->BindBufferStorageRead(self, MaterialBuffer, RHIPipelineStageBits::ComputeShader, "materials");
            r->BindBufferStorageRead(self, LightBuffer, RHIPipelineStageBits::ComputeShader, "lights");
            r->BindBufferStorageRead(self, LightAliasTableBuffer, RHIPipelineStageBits::ComputeShader, "lightAliasTable");
            r->BindTextureSRV(self, EnvMapTex, "envMap", RHIPipelineStageBits::ComputeShader,
                              RHITextureViewDesc{.format = gpu->GetEnvMap()
                                                     ? RHIResourceFormat::R32G32B32A32SignedFloat
                                                     : RHIResourceFormat::R8G8B8A8Unorm,
                                                .range = RHITextureSubresourceRange::Create()});
            r->BindTextureSRV(self, EnvMapConditionalCDF, "envMapConditionalCDF", RHIPipelineStageBits::ComputeShader,
                              RHITextureViewDesc{.format = RHIResourceFormat::R32SignedFloat,
                                                .range = RHITextureSubresourceRange::Create()});
            r->BindBufferStorageRead(self, EnvMapMarginalCDF, RHIPipelineStageBits::ComputeShader, "envMapMarginalCDF");
            r->BindBufferStorageRead(self, SobolMatricesBuffer, RHIPipelineStageBits::ComputeShader, "sobolMatrices");
            r->BindTextureSRV(self, GGXlutE, "ggxLutE", RHIPipelineStageBits::ComputeShader,
                              RHITextureViewDesc{.format = RHIResourceFormat::R32G32SignedFloat,
                                                .range = RHITextureSubresourceRange::Create()});
            r->BindTextureSRV(self, GGXlutEavg, "ggxLutEavg", RHIPipelineStageBits::ComputeShader,
                              RHITextureViewDesc{.format = RHIResourceFormat::R32SignedFloat,
                                                .range = RHITextureSubresourceRange::Create()});
            r->BindTextureSampler(self, TexSampler, "textureSampler");
            r->BindTextureSampler(self, LUTSampler, "lutSampler");
            // Accumulation UAVs (Welford online mean)
            r->BindTextureUAV(self, Diffuse, "diffuse", RHIPipelineStageBits::RayTracingShader,
                              RHITextureViewDesc{.format = RHIResourceFormat::R32G32B32A32SignedFloat,
                                                 .range = RHITextureSubresourceRange::Create()});
            r->BindTextureUAV(self, Specular, "specular", RHIPipelineStageBits::RayTracingShader,
                              RHITextureViewDesc{.format = RHIResourceFormat::R32G32B32A32SignedFloat,
                                                 .range = RHITextureSubresourceRange::Create()});
            r->BindTextureUAV(self, GBuffer0, "gBuffer0", RHIPipelineStageBits::RayTracingShader,
                              RHITextureViewDesc{.format = RHIResourceFormat::R32G32B32A32SignedFloat,
                                                 .range = RHITextureSubresourceRange::Create()});
            r->BindTextureUAV(self, GBuffer1, "gBuffer1", RHIPipelineStageBits::RayTracingShader,
                              RHITextureViewDesc{.format = RHIResourceFormat::R32G32B32A32SignedFloat,
                                                 .range = RHITextureSubresourceRange::Create()});
            r->BindTextureUAV(self, GBuffer2, "gBuffer2", RHIPipelineStageBits::RayTracingShader,
                              RHITextureViewDesc{.format = RHIResourceFormat::R32G32B32A32SignedFloat,
                                                 .range = RHITextureSubresourceRange::Create()});
            r->BindTextureUAV(self, PickIDBuffer, "pickIDBuffer", RHIPipelineStageBits::RayTracingShader,
                              RHITextureViewDesc{.format = RHIResourceFormat::R32Uint,
                                                 .range = RHITextureSubresourceRange::Create()});
            r->BindTextureSRV(self, GGXlutE, "ggxLutE", RHIPipelineStageBits::RayTracingShader,
                              RHITextureViewDesc{.format = RHIResourceFormat::R32G32SignedFloat,
                                                 .range = RHITextureSubresourceRange::Create()});
            r->BindTextureSRV(self, EnvMapTex, "envMapTexture", RHIPipelineStageBits::RayTracingShader,
                              RHITextureViewDesc{.format = gpu->GetEnvMap() ? RHIResourceFormat::R32G32B32A32SignedFloat
                                                                            : RHIResourceFormat::R8G8B8A8Unorm,
                                                 .range = RHITextureSubresourceRange::Create()});
            r->BindTextureSampler(self, EnvMapSampler, "envMapSampler");
            r->BindBufferStorageRead(self, EnvMapMarginalCDF, RHIPipelineStageBits::RayTracingShader, "envMapMarginalCDF");
            r->BindTextureSRV(self, EnvMapConditionalCDF, "envMapConditionalCDF", RHIPipelineStageBits::RayTracingShader,
                              RHITextureViewDesc{.format = RHIResourceFormat::R32SignedFloat,
                                                 .range = RHITextureSubresourceRange::Create()});
            r->BindDescriptorSet(self, "textures", gpu->GetTexturePool()->GetDescriptorSetLayout());
        },
        [=](PassHandle self, Renderer* r, RHICommandList* cmd)
        {
            r->CmdSetPipeline(self, cmd);
            r->CmdBindDescriptorSet(self, cmd, "textures", gpu->GetTexturePool()->GetDescriptorSet());
            if (!renderPaused || !*renderPaused)
            {
                uint32_t tileSide = PTDispatchTileSide(*scene.gsGlobals);
                cmd->TraceRays((w - 1u) / tileSide + 1u, (h - 1u) / tileSide + 1u, 1);
            }
        });

    createPSFullscreenPass(
        renderer, "Blit Image",
        [=](PassHandle self, Renderer* r)
        {
            r->BindShader(self, RHIShaderStageBits::Fragment, "fragMain", Paths::Resolve("data/shaders/EPSBlitPT.spv"),
                          AsBytes(AsSpan(cfg.viewFlags)));
            // Bind all buffers as SRVs — blit pass does compositing + tone map
            auto bindSRV = [&](ResourceHandle h, const char* name)
            {
                r->BindTextureSRV(self, h, name, RHIPipelineStageBits::FragmentShader,
                                  RHITextureViewDesc{.format = RHIResourceFormat::R32G32B32A32SignedFloat,
                                                     .range = RHITextureSubresourceRange::Create()});
            };
            bindSRV(Diffuse, "diffuseTex");
            bindSRV(Specular, "specularTex");
            bindSRV(GBuffer0, "gBuffer0Tex");
            bindSRV(GBuffer1, "gBuffer1Tex");
            bindSRV(GBuffer2, "gBuffer2Tex");
            r->BindTextureSRV(self, PickIDBuffer, "pickIDBuffer", RHIPipelineStageBits::FragmentShader,
                              RHITextureViewDesc{.format = RHIResourceFormat::R32Uint,
                                                 .range = RHITextureSubresourceRange::Create()});
            r->BindBufferUnordered(self, PickResultBuffer, RHIPipelineStageBits::FragmentShader, "pickResult");
            r->BindPushConstant(self, RHIShaderStageBits::Fragment, 0, sizeof(int2));
            r->BindBufferUniform(self, GlobalUBO, RHIPipelineStageBits::FragmentShader, "globalParams");
        },
        [=](PassHandle self, Renderer* r, RHICommandList* cmd)
        { r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Fragment, 0, scene.picking->pendingPixel); });
}