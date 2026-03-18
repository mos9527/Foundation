#include "ImGui.hpp"
#include "Paths.hpp"
#include "Renderer.hpp"
#include <RenderUtils/CSClearBuffer.hpp>
#include <RenderUtils/CSMipGeneration.hpp>
#include <RenderUtils/PSFullscreen.hpp>
using namespace RenderUtils;

void PathTracerSetup(FContext* context, RendererConfig cfg, RendererScene scene)
{
    auto* renderer = context->renderer = Construct<Renderer>(context->allocator,
                                                             RendererDesc{
                                                                 .asyncCompute = true,
                                                                 .pipelineCache = context->psoCache.Get(),
                                                             },
                                                             context->device, context->swapchain, context->allocator);
    auto* gpu = context->gpuScene;
    renderer->BeginSetup();
    scene.gsGlobals->ptAccumualatedFrames = 0u;
    auto GlobalUBO = renderer->CreateResource(
        "Global UBO",
        RHIBufferDesc{.usage = RHIBufferUsageBits::TransferDestination | RHIBufferUsageBits::UniformBuffer,
                      .size = sizeof(UBO)});
    renderer->CreatePass(
        "UBO Update", RHIDeviceQueueType::Graphics, 0u,
        [=](PassHandle self, Renderer* r)
        {
            r->BindBufferCopyDst(self, GlobalUBO);
        },
        [=](PassHandle, Renderer* r, RHICommandList* cmd)
        {
            auto* ubo = r->DerefResource(GlobalUBO).Get<RHIBuffer*>();
            cmd->UpdateBuffer(ubo, 0, AsBytes(AsSpan(*scene.gsGlobals)));
        });
    auto TLAS = renderer->CreateResource("Scene TLAS", gpu->GetTLAS());
    renderer->CreatePass(
        "TLAS Update", RHIDeviceQueueType::Graphics, 0u, [=](PassHandle self, Renderer* r)
        {
            r->BindAccelerationStructureWrite(self, TLAS);
        }, [=](PassHandle, Renderer* r, RHICommandList* cmd)
        {
            gpu->BuildTLAS(cmd, *scene.gsInstances, *scene.gsBLASes, true);
        });
    auto InstanceBuffer = renderer->CreateResource("Instance Buffer", gpu->GetInstanceBuffer());
    auto PrimitiveBuffer = renderer->CreateResource("Primitive Buffer", gpu->GetPrimitiveBuffer());
    auto MaterialBuffer = renderer->CreateResource("Material Buffer", gpu->GetMaterialBuffer());
    auto TexSampler = renderer->CreateSampler({});
    auto [w,h] = renderer->GetSwapchainExtent();
    auto AccumulatedBuffer = renderer->CreateResource("ABuffer", RHITextureDesc{
                                                          .usage = RHITextureUsageBits::StorageImage |
                                                          RHITextureUsageBits::SampledImage,
                                                          .extent = {w, h, 1},
                                                          .format = RHIResourceFormat::R32G32B32A32SignedFloat});
    auto LightingBuffer = renderer->CreateResource("Lighting Buffer", RHITextureDesc{
                                                       .usage = RHITextureUsageBits::StorageImage |
                                                       RHITextureUsageBits::SampledImage,
                                                       .extent = {w, h, 1},
                                                       .format = RHIResourceFormat::R16G16B16A16SignedFloat});

    auto GGXlutE = renderer->CreateResource("GGX LUT E", gpu->GetGGXlutE());
    ResourceHandle EnvMapTex;
    if (gpu->GetEnvMap()) {
        EnvMapTex = renderer->CreateResource("Env Map", gpu->GetEnvMap());
    } else {
        EnvMapTex = renderer->CreateResource("Env Map Fallback", RHITextureDesc{
            .usage = RHITextureUsageBits::SampledImage | RHITextureUsageBits::TransferDestination,
            .extent = {1, 1, 1},
            .format = RHIResourceFormat::R8G8B8A8Unorm});
    }
    auto EnvMapSampler = renderer->CreateSampler({
        .filter = {RHIDeviceSampler::SamplerDesc::Filter::Linear,
                   RHIDeviceSampler::SamplerDesc::Filter::Linear},
    });
    auto LUTSampler = renderer->CreateSampler({
        .addressMode = {
            .u = RHIDeviceSampler::SamplerDesc::AddressMode::ClampToEdge,
            .v = RHIDeviceSampler::SamplerDesc::AddressMode::ClampToEdge,
            .w = RHIDeviceSampler::SamplerDesc::AddressMode::ClampToEdge,
        }
    });
    renderer->CreatePass(
        "Trace", RHIDeviceQueueType::Graphics, 0u,
        [=](PassHandle self, Renderer* r)
        {
            r->BindBufferUniform(self, GlobalUBO, RHIPipelineStageBits::RayTracingShader, "globalParams");
            r->BindAccelerationStructureSRV(self, TLAS, RHIPipelineStageBits::RayTracingShader, "AS");
            r->BindShader(self, RHIShaderStageBits::RayGeneration, "RayGeneration", Paths::Resolve("data/shaders/ERTPathTracer.spv"),
                          AsBytes(AsSpan(cfg.viewFlags)));
            r->BindShader(self, RHIShaderStageBits::RayClosestHit, "RayClosestHit", Paths::Resolve("data/shaders/ERTPathTracer.spv"),
                          AsBytes(AsSpan(cfg.viewFlags)), /*hit group*/ 0);
            r->BindShader(self, RHIShaderStageBits::RayAnyHit, "RayOpacityAnyHit", Paths::Resolve("data/shaders/ERTPathTracer.spv"),
                          AsBytes(AsSpan(cfg.viewFlags)), /*hit group*/ 0);
            r->BindShader(self, RHIShaderStageBits::RayMiss, "RayMiss", Paths::Resolve("data/shaders/ERTPathTracer.spv"),
                          AsBytes(AsSpan(cfg.viewFlags)));
            r->BindShader(self, RHIShaderStageBits::RayAnyHit, "ShadowRayAnyHit", Paths::Resolve("data/shaders/ERTPathTracer.spv"),
                          AsBytes(AsSpan(cfg.viewFlags)), /*hit group*/ 1);
            r->BindShader(self, RHIShaderStageBits::RayMiss, "ShadowRayMiss", Paths::Resolve("data/shaders/ERTPathTracer.spv"),
                          AsBytes(AsSpan(cfg.viewFlags)));
            r->BindBufferStorageRead(self, InstanceBuffer, RHIPipelineStageBits::ComputeShader, "instances");
            r->BindBufferStorageRead(self, PrimitiveBuffer, RHIPipelineStageBits::AllGraphics, "primitives");
            r->BindBufferStorageRead(self, MaterialBuffer, RHIPipelineStageBits::AllGraphics, "materials");
            r->BindTextureSampler(self, TexSampler, "textureSampler");
            r->BindTextureSampler(self, LUTSampler, "lutSampler");
            r->BindTextureUAV(self, AccumulatedBuffer, "accumulation", RHIPipelineStageBits::RayTracingShader,
                              RHITextureViewDesc{.format = RHIResourceFormat::R32G32B32A32SignedFloat,
                                                 .range = RHITextureSubresourceRange::Create()});
            r->BindTextureUAV(self, LightingBuffer, "lighting", RHIPipelineStageBits::RayTracingShader,
                              RHITextureViewDesc{.format = RHIResourceFormat::R16G16B16A16SignedFloat,
                                                 .range = RHITextureSubresourceRange::Create()});
            r->BindTextureSRV(self, GGXlutE, "ggxLutE", RHIPipelineStageBits::RayTracingShader,
                              RHITextureViewDesc{.format = RHIResourceFormat::R32G32SignedFloat,
                                                 .range = RHITextureSubresourceRange::Create()});
            r->BindTextureSRV(self, EnvMapTex, "envMapTexture", RHIPipelineStageBits::RayTracingShader,
                              RHITextureViewDesc{.format = gpu->GetEnvMap()
                                                     ? RHIResourceFormat::R32G32B32A32SignedFloat
                                                     : RHIResourceFormat::R8G8B8A8Unorm,
                                                 .range = RHITextureSubresourceRange::Create()});
            r->BindTextureSampler(self, EnvMapSampler, "envMapSampler");
            r->BindDescriptorSet(self, "textures", gpu->GetTexturePool()->GetDescriptorSetLayout());
        }, [=](PassHandle self, Renderer* r, RHICommandList* cmd)
        {
            RHIExtent2D wh = r->GetSwapchainExtent();
            r->CmdSetPipeline(self, cmd);
            r->CmdBindDescriptorSet(self, cmd, "textures", gpu->GetTexturePool()->GetDescriptorSet());
            cmd->TraceRays(wh.x, wh.y, 1);
        });

    createPSFullscreenPass(
        renderer, "Blit Image",
        [=](PassHandle self, Renderer* r)
        {
            r->BindShader(self, RHIShaderStageBits::Fragment, "fragMain", Paths::Resolve("data/shaders/EPSBlitPT.spv"),
                          AsBytes(AsSpan(cfg.viewFlags)));
            r->BindTextureSRV(self, LightingBuffer, "lighting", RHIPipelineStageBits::FragmentShader,
                              RHITextureViewDesc{.format = RHIResourceFormat::R16G16B16A16SignedFloat,
                                                 .range = RHITextureSubresourceRange::Create()});
            r->BindBufferUniform(self, GlobalUBO, RHIPipelineStageBits::FragmentShader, "globalParams");
        });
    ImGui_ImplFoundation_CreatePass(renderer, "ImGui", false, FSetupDefault{});
    renderer->EndSetup();
}
