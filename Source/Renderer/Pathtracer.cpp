#include <Core/Paths.hpp>
#include <RenderUtils/CSClearBuffer.hpp>
#include <RenderUtils/CSMipGeneration.hpp>
#include <algorithm>
#include "GPUScene.hpp"
#include "Renderer.hpp"
void BuildPathTracerRenderGraph(Renderer* renderer, RendererUBO* globals, RendererResources& gpu,
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
    auto SobolMatricesBuffer = gpu.sobolMatricesBuffer;
    BuildGPUSceneUpdatePasses(renderer, gpu, GlobalUBO, true);
    auto TexSampler = renderer->CreateSampler(MakeTextureSamplerDesc(cfg));
    uint32_t w = std::max(renderExtent.x, 1u);
    uint32_t h = std::max(renderExtent.y, 1u);
    constexpr RHIResourceFormat kPathTracerAOVFormat = RHIResourceFormat::R32G32B32A32SignedFloat;

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
    auto Depth = renderer->CreateResource("Depth",
                                          RHITextureDesc{.usage = RHITextureUsageBits::StorageImage |
                                                             RHITextureUsageBits::SampledImage |
                                                             RHITextureUsageBits::TransferSource,
                                                         .extent = {w, h, 1},
                                                         .format = RHIResourceFormat::R32SignedFloat});
    // Instance ID map: R32_UINT, written every frame on primary hit (no accumulation)
    auto InstanceIDBuffer = renderer->CreateResource(
        "Instance ID Buffer",
        RHITextureDesc{.usage = RHITextureUsageBits::StorageImage | RHITextureUsageBits::SampledImage,
                       .extent = {w, h, 1},
                       .format = RHIResourceFormat::R32Uint});
    auto AdaptiveAux = renderer->CreateResource("Adaptive Aux",
                                                RHITextureDesc{.usage = RHITextureUsageBits::StorageImage |
                                                                    RHITextureUsageBits::SampledImage |
                                                                    RHITextureUsageBits::TransferSource,
                                                               .extent = {w, h, 1},
                                                               .format = kPathTracerAOVFormat});
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
    const char* passName = shaderExecutionReordering ? "Trace (SER)" : "Trace (Compute)";
    LOG(Pathtracer, LogInfo, "{} will be used for integration", passName);
    renderer->CreatePass(
        passName, RHIDeviceQueueType::Graphics, 0u,
        [=](PassHandle self, Renderer* r)
    {
        const auto pipelineStage = shaderExecutionReordering
            ? RHIPipelineStageBits::RayTracingShader
            : RHIPipelineStageBits::ComputeShader;
        r->BindBufferUniform(self, GlobalUBO, pipelineStage, "globalParams");
        r->BindAccelerationStructureSRV(self, TLAS, pipelineStage, "AS");
        const uint ptCompileOptions =
            PTPackCompileOptions(cfg.ptSampler, cfg.forceTextureLOD0, cfg.lightSamplerMode, cfg.energyCompensation);
        const auto shader = PathsResolve(
            !shaderExecutionReordering ? "Data/Shaders/ERTPathTracer.spv" : "Data/Shaders/ERTPathTracer_SER.spv"
        );
        if (shaderExecutionReordering)
        {
            r->BindShader(self, RHIShaderStageBits::RayGeneration, "RayGeneration", shader,
                          AsBytes(AsSpan(ptCompileOptions)));
        }
        else
        {
            r->BindShader(self, RHIShaderStageBits::Compute, "ComputeMain", shader,
                          AsBytes(AsSpan(ptCompileOptions)));
        }

        r->BindBufferStorageRead(self, PrimitiveBuffer, pipelineStage, "primitives");
        r->BindBufferStorageRead(self, DynamicPrimitiveBuffer, pipelineStage, "dynamicPrimitives");
        r->BindBufferStorageRead(self, InstanceBuffer, pipelineStage, "instances");
        r->BindBufferStorageRead(self, MaterialBuffer, pipelineStage, "materials");
        r->BindBufferStorageRead(self, LightBuffer, pipelineStage, "lights");
        r->BindBufferStorageRead(self, LightBVHNodeBuffer, pipelineStage, "lightBVHNodes");
        r->BindBufferStorageRead(self, LightBVHLightIndexBuffer, pipelineStage, "lightBVHLightIndices");
        r->BindBufferStorageRead(self, LightBVHBitmaskBuffer, pipelineStage, "lightBVHBitmasks");
        r->BindBufferStorageRead(self, SobolMatricesBuffer, pipelineStage, "sobolMatrices");
        r->BindTextureSampler(self, TexSampler, "textureSampler");
        r->BindTextureSampler(self, LUTSampler, "lutSampler");
        // Accumulation UAVs
        r->BindTextureUAV(self, Diffuse, "diffuse", pipelineStage,
                          RHITextureViewDesc{.format = kPathTracerAOVFormat,
                                             .range = RHITextureSubresourceRange::Create()});
        r->BindTextureUAV(self, Specular, "specular", pipelineStage,
                          RHITextureViewDesc{.format = kPathTracerAOVFormat,
                                             .range = RHITextureSubresourceRange::Create()});
        r->BindTextureUAV(self, Depth, "depth", pipelineStage,
                          RHITextureViewDesc{.format = RHIResourceFormat::R32SignedFloat,
                                             .range = RHITextureSubresourceRange::Create()});
        r->BindTextureUAV(self, InstanceIDBuffer, "instanceIDBuffer", pipelineStage,
                          RHITextureViewDesc{.format = RHIResourceFormat::R32Uint,
                                             .range = RHITextureSubresourceRange::Create()});
        r->BindTextureUAV(self, AdaptiveAux, "adaptiveAux", pipelineStage,
                          RHITextureViewDesc{.format = kPathTracerAOVFormat,
                                             .range = RHITextureSubresourceRange::Create()});
        r->BindTextureSampler(self, EnvMapSampler, "envMapSampler");
        r->BindDescriptorSet(self, "textures", gpu.textures2D->GetDescriptorSetLayout());
        r->BindDescriptorSet(self, "textures3D", gpu.textures3D->GetDescriptorSetLayout());
    },
    [=](PassHandle self, Renderer* r, RHICommandList* cmd)
    {
        r->CmdSetPipeline(self, cmd);
        r->CmdBindDescriptorSet(self, cmd, "textures", gpu.textures2D->GetDescriptorSet());
        r->CmdBindDescriptorSet(self, cmd, "textures3D", gpu.textures3D->GetDescriptorSet());
        bool canTrace = (!cfg.ptRenderPaused || !*cfg.ptRenderPaused);
        if (canTrace)
        {
            if (shaderExecutionReordering)
                cmd->TraceRays(w, h, 1);
            else
                cmd->Dispatch((w - 1) / 8 + 1, (h - 1) / 8 + 1, 1);
        }
    });

    renderer->CreatePass(
        "Adaptive Filter X", RHIDeviceQueueType::Graphics, 0u,
        [=](PassHandle self, Renderer* r)
        {
            if (globals->adaptiveThreshold > 0.0f)
                r->MakePassUncullable(self);
            r->BindShader(self, RHIShaderStageBits::Compute, "ComputeMain", PathsResolve("Data/Shaders/ECSAdaptiveFilter.spv"),
                          AsBytes(AsSpan(0u))); // Pass 0
            r->BindTextureUAV(self, AdaptiveAux, "adaptiveAux", RHIPipelineStageBits::ComputeShader,
                              RHITextureViewDesc{.format = kPathTracerAOVFormat,
                                                 .range = RHITextureSubresourceRange::Create()});
            r->BindBufferUniform(self, GlobalUBO, RHIPipelineStageBits::ComputeShader, "globalParams");
        },
        [=](PassHandle self, Renderer* r, RHICommandList* cmd)
        {
            if (globals->adaptiveThreshold > 0.0f)
            {
                r->CmdSetPipeline(self, cmd);
                cmd->Dispatch((w - 1) / 8 + 1, (h - 1) / 8 + 1, 1);
            }
        });

    renderer->CreatePass(
        "Adaptive Filter Y", RHIDeviceQueueType::Graphics, 0u,
        [=](PassHandle self, Renderer* r)
        {
            if (globals->adaptiveThreshold > 0.0f)
                r->MakePassUncullable(self);
            r->BindShader(self, RHIShaderStageBits::Compute, "ComputeMain", PathsResolve("Data/Shaders/ECSAdaptiveFilter.spv"),
                          AsBytes(AsSpan(1u))); // Pass 1
            r->BindTextureUAV(self, AdaptiveAux, "adaptiveAux", RHIPipelineStageBits::ComputeShader,
                              RHITextureViewDesc{.format = kPathTracerAOVFormat,
                                                 .range = RHITextureSubresourceRange::Create()});
            r->BindBufferUniform(self, GlobalUBO, RHIPipelineStageBits::ComputeShader, "globalParams");
        },
        [=](PassHandle self, Renderer* r, RHICommandList* cmd)
        {
            if (globals->adaptiveThreshold > 0.0f)
            {
                r->CmdSetPipeline(self, cmd);
                cmd->Dispatch((w - 1) / 8 + 1, (h - 1) / 8 + 1, 1);
            }
        });

    out.extent = {w, h};
    out.aovFormat = kPathTracerAOVFormat;
    out.diffuse = Diffuse;
    out.specular = Specular;
    out.depth = Depth;
    out.instanceID = InstanceIDBuffer;
}