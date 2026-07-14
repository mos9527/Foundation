#include <Core/Paths.hpp>
#include <RenderUtils/CSClearBuffer.hpp>
#include <RenderUtils/CSMipGeneration.hpp>
#include <algorithm>
#include "GPUScene.hpp"
#include "Renderer.hpp"
void BuildPathTracerRenderGraph(Renderer* renderer, RendererUBO* globals, GPUScene* gpu,
                                RendererConfig const& cfg, RendererOutputs& out)
{
    CHECK(renderer);
    CHECK(globals);
    CHECK(gpu);
    CHECK_MSG(renderer->GetDevice()->GetCapabilities().raytracingInline, "Pathtracer requires Ray Query support");
    out = {};
    globals->ptAccumulatedFrames = 0u;
    RHIExtent2D renderExtent = cfg.renderExtent;
    if (renderExtent.x == 0u || renderExtent.y == 0u)
        renderExtent = renderer->GetSwapchainExtent();
    // Framebuffer extents are renderer-owned: stamped from the resolved cfg.renderExtent
    // (matching the actual render-target sizes) so the host UBO and GPU UBO stay in sync.
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
    CHECK(gpu->GetTLAS() && "Pathtracer requires a TLAS to be built/updated from.");
    ResourceHandle TLAS = kInvalidHandle;
    TLAS = renderer->CreateResource("Scene TLAS", gpu->GetTLAS());
    renderer->CreatePass(
        "TLAS/BLAS Update", RHIDeviceQueueType::Graphics, 0u, [=](PassHandle self, Renderer* r)
        { r->BindAccelerationStructureWrite(self, TLAS); }, [=](PassHandle, Renderer* r, RHICommandList* cmd)
        {
            if (gpu->HasDynamicGeometry())
                gpu->BuildBLAS(cmd);
            (void)gpu->BuildTLAS(cmd, true);
        });
    /* Instance and Primitive buffers */
    auto PrimitiveBuffer = renderer->CreateResource("Primitive Buffer", gpu->GetPrimitiveBuffer());
    auto DynamicPrimitiveBuffer = renderer->CreateResource("Dynamic Primitive Buffer", gpu->GetDynamicPrimitiveBuffer());
    auto InstanceBuffer = renderer->CreateResource("Instance Buffer", gpu->GetInstanceBuffer());
    auto MaterialBuffer = renderer->CreateResource("Material Buffer", gpu->GetMaterialBuffer());
    auto LightBuffer = renderer->CreateResource("Light Buffer", gpu->GetLightBuffer());
    auto LightBVHNodeBuffer = renderer->CreateResource("Light BVH Nodes", gpu->GetLightBVHNodeBuffer());
    auto LightBVHLightIndexBuffer =
        renderer->CreateResource("Light BVH Light Indices", gpu->GetLightBVHLightIndexBuffer());
    auto LightBVHBitmaskBuffer = renderer->CreateResource("Light BVH Bitmasks", gpu->GetLightBVHBitmaskBuffer());
    auto LightBVHGlobalIndexBuffer =
        renderer->CreateResource("Light BVH Global Indices", gpu->GetLightBVHGlobalIndexBuffer());
    auto LightBVHNodeIndexBuffer =
        renderer->CreateResource("Light BVH Node Indices", gpu->GetLightBVHNodeIndexBuffer());
    auto SobolMatricesBuffer = renderer->CreateResource("Sobol Matrices Buffer", gpu->GetSobolMatricesBuffer());
    auto TexSampler = renderer->CreateSampler(MakeTextureSamplerDesc(cfg));
    uint32_t w = std::max(renderExtent.x, 1u);
    uint32_t h = std::max(renderExtent.y, 1u);
    constexpr RHIResourceFormat kPathTracerAOVFormat = RHIResourceFormat::R32G32B32A32SignedFloat;

    struct LightBVHRefitPush
    {
        uint32_t firstNodeOffset;
        uint32_t nodeCount;
    };

    renderer->CreatePass(
        "Light BVH Refit Leaves", RHIDeviceQueueType::Graphics, 0u,
        [=](PassHandle self, Renderer* r)
        {
            r->BindBufferUniform(self, GlobalUBO, RHIPipelineStageBits::ComputeShader, "globalParams");
            r->BindBufferStorageRead(self, LightBuffer, RHIPipelineStageBits::ComputeShader, "lights");
            r->BindBufferUnordered(self, LightBVHNodeBuffer, RHIPipelineStageBits::ComputeShader, "lightBVHNodes");
            r->BindBufferStorageRead(self, LightBVHLightIndexBuffer, RHIPipelineStageBits::ComputeShader,
                                     "lightBVHLightIndices");
            r->BindBufferStorageRead(self, LightBVHNodeIndexBuffer, RHIPipelineStageBits::ComputeShader,
                                     "gNodeIndices");
            r->BindShader(self, RHIShaderStageBits::Compute, "updateLeafNodes",
                          PathsResolve("Data/Shaders/ECSLightBVHRefit.spv"));
            r->BindPushConstant(self, RHIShaderStageBits::Compute, 0, sizeof(LightBVHRefitPush));
        },
        [=](PassHandle self, Renderer* r, RHICommandList* cmd)
        {
            if (!gpu->NeedsLightBVHRefit())
                return;
            uint32_t levels = gpu->GetLightBVHRefitLevelCount();
            if (levels == 0u)
                return;
            uint32_t leafLevel = levels - 1u;
            uint32_t count = gpu->GetLightBVHRefitLevelNodeCount(leafLevel);
            if (count == 0u)
                return;
            LightBVHRefitPush pc{gpu->GetLightBVHFirstNodeIndex() + gpu->GetLightBVHRefitLevelOffset(leafLevel),
                                 count};
            r->CmdSetPipeline(self, cmd);
            r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Compute, 0, pc);
            cmd->Dispatch((count + 255u) / 256u, 1, 1);
        });

    renderer->CreatePass(
        "Light BVH Refit Internals", RHIDeviceQueueType::Graphics, 0u,
        [=](PassHandle self, Renderer* r)
        {
            r->BindBufferUniform(self, GlobalUBO, RHIPipelineStageBits::ComputeShader, "globalParams");
            r->BindBufferStorageRead(self, LightBuffer, RHIPipelineStageBits::ComputeShader, "lights");
            r->BindBufferUnordered(self, LightBVHNodeBuffer, RHIPipelineStageBits::ComputeShader, "lightBVHNodes");
            r->BindBufferStorageRead(self, LightBVHLightIndexBuffer, RHIPipelineStageBits::ComputeShader,
                                     "lightBVHLightIndices");
            r->BindBufferStorageRead(self, LightBVHNodeIndexBuffer, RHIPipelineStageBits::ComputeShader,
                                     "gNodeIndices");
            r->BindShader(self, RHIShaderStageBits::Compute, "updateInternalNodes",
                          PathsResolve("Data/Shaders/ECSLightBVHRefit.spv"));
            r->BindPushConstant(self, RHIShaderStageBits::Compute, 0, sizeof(LightBVHRefitPush));
        },
        [=](PassHandle self, Renderer* r, RHICommandList* cmd)
        {
            if (!gpu->NeedsLightBVHRefit())
                return;
            uint32_t levels = gpu->GetLightBVHRefitLevelCount();
            if (levels < 2u)
                return;
            r->CmdSetPipeline(self, cmd);
            for (int level = static_cast<int>(levels) - 2; level >= 0; --level)
            {
                uint32_t count = gpu->GetLightBVHRefitLevelNodeCount(static_cast<uint32_t>(level));
                if (count == 0u)
                    continue;
                LightBVHRefitPush pc{
                    gpu->GetLightBVHFirstNodeIndex() + gpu->GetLightBVHRefitLevelOffset(static_cast<uint32_t>(level)),
                    count};
                r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Compute, 0, pc);
                cmd->Dispatch((count + 255u) / 256u, 1, 1);
                if (level > 0)
                {
                    cmd->BeginTransition();
                    cmd->SetBufferTransition(
                        gpu->GetLightBVHNodeBuffer(),
                        {.srcAccess = RHIResourceAccessBits::ShaderWrite,
                         .dstAccess = RHIResourceAccessBits::ShaderRead | RHIResourceAccessBits::ShaderWrite,
                         .srcStage = RHIPipelineStageBits::ComputeShader,
                         .dstStage = RHIPipelineStageBits::ComputeShader});
                    cmd->EndTransition();
                }
            }
        });

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
        const uint ptCompileOptions = PTPackCompileOptions(cfg.ptSampler, cfg.forceTextureLOD0);
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
        r->BindBufferStorageRead(self, LightBVHGlobalIndexBuffer, pipelineStage, "lightBVHGlobalIndices");
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
        r->BindDescriptorSet(self, "textures", gpu->GetTexture2DPool()->GetDescriptorSetLayout());
        r->BindDescriptorSet(self, "textures3D", gpu->GetTexture3DPool()->GetDescriptorSetLayout());
    },
    [=](PassHandle self, Renderer* r, RHICommandList* cmd)
    {
        r->CmdSetPipeline(self, cmd);
        r->CmdBindDescriptorSet(self, cmd, "textures", gpu->GetTexture2DPool()->GetDescriptorSet());
        r->CmdBindDescriptorSet(self, cmd, "textures3D", gpu->GetTexture3DPool()->GetDescriptorSet());
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