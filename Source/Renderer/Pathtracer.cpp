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
    CHECK(renderer->GetDevice()->GetCapabilities().raytracingPipeline);
    out = {};
    globals->ptAccumulatedFrames = 0u;
    RHIExtent2D renderExtent = cfg.renderExtent;
    if (renderExtent.x == 0u || renderExtent.y == 0u)
        renderExtent = renderer->GetSwapchainExtent();
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
        "TLAS/BLAS Update", RHIDeviceQueueType::Compute, 0u, [=](PassHandle self, Renderer* r)
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
    auto LightAliasTableBuffer = renderer->CreateResource("Light Alias Table Buffer", gpu->GetLightAliasTableBuffer());
    auto SobolMatricesBuffer = renderer->CreateResource("Sobol Matrices Buffer", gpu->GetSobolMatricesBuffer());
    auto TexSampler = renderer->CreateSampler(MakeTextureSamplerDesc(cfg));
    uint32_t w = std::max(renderExtent.x, 1u);
    uint32_t h = std::max(renderExtent.y, 1u);
    // AOV buffers
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
    ResourceHandle EnvMapSampler = renderer->CreateSampler({
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
        using RTHitGroupType = RHIPipelineState::PipelineStateDesc::RayTracingHitGroupType;
        r->BindBufferUniform(self, GlobalUBO, RHIPipelineStageBits::RayTracingShader, "globalParams");
        r->BindAccelerationStructureSRV(self, TLAS, RHIPipelineStageBits::RayTracingShader, "AS");
        const bool shaderExecutionReordering =
            cfg.ptShaderExecutionReordering && r->GetDevice()->GetCapabilities().shaderExecutionReordering;
        const uint ptCompileOptions = PTPackCompileOptions(cfg.ptSampler, cfg.forceTextureLOD0);
        const auto shader = PathsResolve(
            !shaderExecutionReordering ? "Data/Shaders/ERTPathTracer.spv" : "Data/Shaders/ERTPathTracer_SER.spv"
        );
        LOG(PT, LogInfo, "Loading PT Shader: {}", shader);
        r->BindShader(self, RHIShaderStageBits::RayGeneration, "RayGeneration", shader,
                      AsBytes(AsSpan(ptCompileOptions)));
        r->BindShader(self, RHIShaderStageBits::RayClosestHit, "RayClosestHit",
                      shader, AsBytes(AsSpan(ptCompileOptions)), /*hit group*/ 0);
        r->BindShader(self, RHIShaderStageBits::RayAnyHit, "RayOpacityAnyHit",
                      shader, AsBytes(AsSpan(ptCompileOptions)), /*hit group*/ 0);
        r->BindShader(self, RHIShaderStageBits::RayMiss, "RayMiss",
                      shader, AsBytes(AsSpan(ptCompileOptions)));
        r->BindShader(self, RHIShaderStageBits::RayAnyHit, "ShadowRayAnyHit",
                      shader, AsBytes(AsSpan(ptCompileOptions)), /*hit group*/ 1);
        r->BindShader(self, RHIShaderStageBits::RayMiss, "ShadowRayMiss",
                      shader, AsBytes(AsSpan(ptCompileOptions)));
        r->BindShader(self, RHIShaderStageBits::RayAnyHit, "BSSRDFQueryAnyHit",
                      shader, AsBytes(AsSpan(ptCompileOptions)), /*hit group*/ 2);
        r->BindShader(self, RHIShaderStageBits::RayMiss, "BSSRDFQueryMiss",
                      shader, AsBytes(AsSpan(ptCompileOptions)));
        r->BindShader(self, RHIShaderStageBits::RayIntersection, "RectLightIntersection",
                      shader, AsBytes(AsSpan(ptCompileOptions)), kRectLightSBTOffset,
                      RTHitGroupType::Procedural);
        r->BindShader(self, RHIShaderStageBits::RayClosestHit, "RectLightClosestHit",
                      shader, AsBytes(AsSpan(ptCompileOptions)), kRectLightSBTOffset,
                      RTHitGroupType::Procedural);
        r->BindShader(self, RHIShaderStageBits::RayIntersection, "DiskLightIntersection",
                      shader, AsBytes(AsSpan(ptCompileOptions)), kDiskLightSBTOffset,
                      RTHitGroupType::Procedural);
        r->BindShader(self, RHIShaderStageBits::RayClosestHit, "DiskLightClosestHit",
                      shader, AsBytes(AsSpan(ptCompileOptions)), kDiskLightSBTOffset,
                      RTHitGroupType::Procedural);
        r->BindShader(self, RHIShaderStageBits::RayIntersection, "CurveIntersection",
                      shader, AsBytes(AsSpan(ptCompileOptions)), kCurveSBTOffset,
                      RTHitGroupType::Procedural);
        r->BindShader(self, RHIShaderStageBits::RayClosestHit, "CurveClosestHit",
                      shader, AsBytes(AsSpan(ptCompileOptions)), kCurveSBTOffset,
                      RTHitGroupType::Procedural);
        r->BindShader(self, RHIShaderStageBits::RayIntersection, "CurveIntersection",
                      shader, AsBytes(AsSpan(ptCompileOptions)), kCurveSBTOffset + 1u,
                      RTHitGroupType::Procedural);
        r->BindShader(self, RHIShaderStageBits::RayAnyHit, "CurveShadowAnyHit",
                      shader, AsBytes(AsSpan(ptCompileOptions)), kCurveSBTOffset + 1u,
                      RTHitGroupType::Procedural);
        r->BindBufferStorageRead(self, PrimitiveBuffer, RHIPipelineStageBits::ComputeShader, "primitives");
        r->BindBufferStorageRead(self, DynamicPrimitiveBuffer, RHIPipelineStageBits::ComputeShader, "dynamicPrimitives");
        r->BindBufferStorageRead(self, InstanceBuffer, RHIPipelineStageBits::ComputeShader, "instances");
        r->BindBufferStorageRead(self, MaterialBuffer, RHIPipelineStageBits::ComputeShader, "materials");
        r->BindBufferStorageRead(self, LightBuffer, RHIPipelineStageBits::ComputeShader, "lights");
        r->BindBufferStorageRead(self, LightAliasTableBuffer, RHIPipelineStageBits::ComputeShader, "lightAliasTable");
        r->BindBufferStorageRead(self, SobolMatricesBuffer, RHIPipelineStageBits::ComputeShader, "sobolMatrices");
        r->BindTextureSampler(self, TexSampler, "textureSampler");
        r->BindTextureSampler(self, LUTSampler, "lutSampler");
        // Accumulation UAVs
        r->BindTextureUAV(self, Diffuse, "diffuse", RHIPipelineStageBits::RayTracingShader,
                          RHITextureViewDesc{.format = RHIResourceFormat::R32G32B32A32SignedFloat,
                                             .range = RHITextureSubresourceRange::Create()});
        r->BindTextureUAV(self, Specular, "specular", RHIPipelineStageBits::RayTracingShader,
                          RHITextureViewDesc{.format = RHIResourceFormat::R32G32B32A32SignedFloat,
                                             .range = RHITextureSubresourceRange::Create()});
        r->BindTextureUAV(self, Depth, "depth", RHIPipelineStageBits::RayTracingShader,
                          RHITextureViewDesc{.format = RHIResourceFormat::R32SignedFloat,
                                             .range = RHITextureSubresourceRange::Create()});
        r->BindTextureUAV(self, InstanceIDBuffer, "instanceIDBuffer", RHIPipelineStageBits::RayTracingShader,
                          RHITextureViewDesc{.format = RHIResourceFormat::R32Uint,
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
            uint32_t tileSide = PTDispatchTileSide(*globals);
            cmd->TraceRays((w - 1u) / tileSide + 1u, (h - 1u) / tileSide + 1u, 1);
        }
    });

    out.extent = {w, h};
    out.diffuse = Diffuse;
    out.specular = Specular;
    out.depth = Depth;
    out.instanceID = InstanceIDBuffer;
}