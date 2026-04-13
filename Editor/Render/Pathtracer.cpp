#include <RenderUtils/CSClearBuffer.hpp>
#include <RenderUtils/CSMipGeneration.hpp>
#include <RenderUtils/PSFullscreen.hpp>
#include "../ImGui.hpp"
#include "../Paths.hpp"
#include "Editor/EditorState.hpp"
#include "Render.hpp"
using namespace RenderUtils;

void PathTracerSetup(FContext* context, RendererConfig cfg, RendererScene scene, PTReadbackHandles& outHandles)
{
    CHECK(context->device->GetCapabilities().raytracingPipeline);
    auto* renderer = context->renderer = Construct<Renderer>(context->allocator,
                                                             RendererDesc{
                                                                 .asyncCompute = true,
                                                                 .pipelineCache = context->psoCache.Get(),
                                                             },
                                                             context->device, context->swapchain, context->allocator);
    auto* gpu = context->gpuScene;
    renderer->BeginSetup();
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
        { gpu->BuildTLAS(cmd, *scene.gsInstances, *scene.gsBLASes, true); });
    auto InstanceBuffer = renderer->CreateResource("Instance Buffer", gpu->GetInstanceBuffer());
    auto PrimitiveBuffer = renderer->CreateResource("Primitive Buffer", gpu->GetPrimitiveBuffer());
    auto MaterialBuffer = renderer->CreateResource("Material Buffer", gpu->GetMaterialBuffer());
    auto TexSampler = renderer->CreateSampler({});
    auto [w, h] = renderer->GetSwapchainExtent();

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
    outHandles.diffuse = Diffuse;
    outHandles.specular = Specular;
    outHandles.pickResultBuffer = PickResultBuffer;

    auto GGXlutE = renderer->CreateResource("GGX LUT E", gpu->GetGGXlutE());
    ResourceHandle EnvMapTex;
    if (gpu->GetEnvMap())
    {
        EnvMapTex = renderer->CreateResource("Env Map", gpu->GetEnvMap());
    }
    else
    {
        EnvMapTex = renderer->CreateResource(
            "Env Map Fallback",
            RHITextureDesc{.usage = RHITextureUsageBits::SampledImage | RHITextureUsageBits::TransferDestination,
                           .extent = {1, 1, 1},
                           .format = RHIResourceFormat::R8G8B8A8Unorm});
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
            r->BindBufferStorageRead(self, InstanceBuffer, RHIPipelineStageBits::ComputeShader, "instances");
            r->BindBufferStorageRead(self, PrimitiveBuffer, RHIPipelineStageBits::AllGraphics, "primitives");
            r->BindBufferStorageRead(self, MaterialBuffer, RHIPipelineStageBits::AllGraphics, "materials");
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
            r->BindDescriptorSet(self, "textures", gpu->GetTexturePool()->GetDescriptorSetLayout());
        },
        [=](PassHandle self, Renderer* r, RHICommandList* cmd)
        {
            RHIExtent2D wh = r->GetSwapchainExtent();
            r->CmdSetPipeline(self, cmd);
            r->CmdBindDescriptorSet(self, cmd, "textures", gpu->GetTexturePool()->GetDescriptorSet());
            if (!GRenderImageTask.renderPaused)
                cmd->TraceRays(wh.x, wh.y, 1);
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
        { r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Fragment, 0, *scene.gsPickPixel); });

    ImGui_ImplFoundation_CreatePass(renderer, "ImGui", false, FSetupDefault{});
    renderer->EndSetup();
}
