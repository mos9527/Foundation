#include "ImGui.hpp"
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
        "UBO Update & Init", RHIDeviceQueueType::Graphics, 0u,
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
                                                          .format = RHIResourceFormat::R16G16B16A16SignedFloat});
    auto HistogramBins = renderer->CreateResource(
        "Histogram", RHIBufferDesc{.usage = RHIBufferUsageBits::StorageBuffer | RHIBufferUsageBits::TransferDestination,
                                   .size = sizeof(uint32_t) * 64});
    auto LightingAverageLuma = renderer->CreateResource("Lighting Average Luminance",
                                                        RHIBufferDesc{.usage = RHIBufferUsageBits::StorageBuffer,
                                                                      .size = sizeof(float)});
    renderer->CreatePass(
        "Trace", RHIDeviceQueueType::Graphics, 0u,
        [=](PassHandle self, Renderer* r)
        {
            r->BindBufferUniform(self, GlobalUBO, RHIPipelineStageBits::RayTracingShader, "globalParams");
            r->BindAccelerationStructureSRV(self, TLAS, RHIPipelineStageBits::RayTracingShader, "AS");
            r->BindShader(self, RHIShaderStageBits::RayGeneration, "RayGeneration", "data/shaders/ERTPathTracer.spv",
                          AsBytes(AsSpan(cfg.viewFlags)));
            r->BindShader(self, RHIShaderStageBits::RayClosestHit, "RayClosestHit", "data/shaders/ERTPathTracer.spv",
                          AsBytes(AsSpan(cfg.viewFlags)));
            r->BindShader(self, RHIShaderStageBits::RayMiss, "RayMiss", "data/shaders/ERTPathTracer.spv",
                          AsBytes(AsSpan(cfg.viewFlags)));
            r->BindBufferStorageRead(self, InstanceBuffer, RHIPipelineStageBits::ComputeShader, "instances");
            r->BindBufferStorageRead(self, PrimitiveBuffer, RHIPipelineStageBits::AllGraphics, "primitives");
            r->BindBufferStorageRead(self, MaterialBuffer, RHIPipelineStageBits::AllGraphics, "materials");
            r->BindTextureSampler(self, TexSampler, "textureSampler");
            r->BindDescriptorSet(self, "textures", gpu->GetTexturePool()->GetDescriptorSetLayout());
            r->BindTextureUAV(self, AccumulatedBuffer, "accumulation", RHIPipelineStageBits::RayTracingShader,
                              RHITextureViewDesc{.format = RHIResourceFormat::R16G16B16A16SignedFloat,
                                                 .range = RHITextureSubresourceRange::Create()});

        }, [=](PassHandle self, Renderer* r, RHICommandList* cmd)
        {
            RHIExtent2D wh = r->GetSwapchainExtent();
            r->CmdSetPipeline(self, cmd);
            r->CmdBindDescriptorSet(self, cmd, "textures", gpu->GetTexturePool()->GetDescriptorSet());
            cmd->TraceRays(wh.x, wh.y, 1);
        });
    renderer->CreatePass(
        "Histogram Binning", RHIDeviceQueueType::Graphics, 0u,
        [=](PassHandle self, Renderer* r)
        {
            r->BindShader(self, RHIShaderStageBits::Compute, "main", "data/shaders/ECSHistogramBinning.spv");
            r->BindBufferUniform(self, GlobalUBO, RHIPipelineStageBits::ComputeShader, "globalParams");
            r->BindTextureSRV(self, AccumulatedBuffer, "lighting", RHIPipelineStageBits::ComputeShader,
                              RHITextureViewDesc{.format = RHIResourceFormat::R16G16B16A16SignedFloat,
                                                 .range = RHITextureSubresourceRange::Create()});
            r->BindBufferUnordered(self, HistogramBins, RHIPipelineStageBits::ComputeShader, "bins");
        },
        [=](PassHandle self, Renderer* r, RHICommandList* cmd)
        {
            RHIExtent2D wh = r->GetSwapchainExtent();
            r->CmdSetPipeline(self, cmd);
            r->CmdDispatch(self, cmd, {wh.x, wh.y, 1});
        });
    renderer->CreatePass(
        "Histogram Binning Reduce", RHIDeviceQueueType::Graphics, 0u,
        [=](PassHandle self, Renderer* r)
        {
            r->BindShader(self, RHIShaderStageBits::Compute, "reduce", "data/shaders/ECSHistogramReduce.spv");
            r->BindBufferUniform(self, GlobalUBO, RHIPipelineStageBits::ComputeShader, "globalParams");
            r->BindBufferStorageRead(self, HistogramBins, RHIPipelineStageBits::ComputeShader, "bins");
            r->BindBufferUnordered(self, LightingAverageLuma, RHIPipelineStageBits::ComputeShader, "output");
        },
        [=](PassHandle self, Renderer* r, RHICommandList* cmd)
        {
            r->CmdSetPipeline(self, cmd);
            cmd->Dispatch(1, 1, 1);
        });
    createPSFullscreenPass(
        renderer, "Blit Image",
        [=](PassHandle self, Renderer* r)
        {
            r->BindShader(self, RHIShaderStageBits::Fragment, "fragMain", "data/shaders/EPSBlitPT.spv",
                          AsBytes(AsSpan(cfg.viewFlags)));
            r->BindTextureSRV(self, AccumulatedBuffer, "lighting", RHIPipelineStageBits::FragmentShader,
                              RHITextureViewDesc{.format = RHIResourceFormat::R16G16B16A16SignedFloat,
                                                 .range = RHITextureSubresourceRange::Create()});
            r->BindBufferStorageRead(self, LightingAverageLuma, RHIPipelineStageBits::FragmentShader, "sceneLuma");
        });
    ImGui_ImplFoundation_CreatePass(renderer, "ImGui", false, FSetupDefault{});
    renderer->EndSetup();
}
