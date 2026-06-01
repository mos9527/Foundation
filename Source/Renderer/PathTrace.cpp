#include <Core/Paths.hpp>
#include <RenderUtils/CSClearBuffer.hpp>
#include <RenderUtils/CSMipGeneration.hpp>
#include <algorithm>
#include "GPUScene.hpp"
#include "RenderUtils/PSFullscreen.hpp"
#include "Renderer.hpp"
using namespace RenderUtils;
using Foundation::Core::PathsResolve;

void BuildPathTracerRenderGraph(Renderer* renderer, GPUScene* gpu, RendererConfig cfg, RendererScene scene,
                                RHIExtent2D renderExtent, RendererHandles& outHandles, bool const* renderPaused)
{
    CHECK(renderer);
    CHECK(gpu);
    CHECK(renderer->GetDevice()->GetCapabilities().raytracingPipeline);
    scene.gsGlobals->ptAccumulatedFrames = 0u;
    gpu->FillGlobals(*scene.gsGlobals, cfg.enableHDR);
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
    bool hasTLAS = gpu->GetTLAS() != nullptr;
    ResourceHandle TLAS = kInvalidHandle;
    if (hasTLAS)
    {
        TLAS = renderer->CreateResource("Scene TLAS", gpu->GetTLAS());
        renderer->CreatePass(
            "TLAS Update", RHIDeviceQueueType::Graphics, 0u, [=](PassHandle self, Renderer* r)
            { r->BindAccelerationStructureWrite(self, TLAS); }, [=](PassHandle, Renderer* r, RHICommandList* cmd)
            {
                auto result = gpu->BuildTLAS(cmd, true);
                if (result == GPUScene::TLASBuildResult::NeedsRendererRebuild && scene.rendererRebuildRequested)
                    *scene.rendererRebuildRequested = true;
            });
    }
    /* Instance and Primitive buffers */
    auto PrimitiveBuffer = renderer->CreateResource("Primitive Buffer", gpu->GetPrimitiveBuffer());
    auto InstanceBuffer = renderer->CreateResource("Instance Buffer", gpu->GetInstanceBuffer());
    auto MaterialBuffer = renderer->CreateResource("Material Buffer", gpu->GetMaterialBuffer());
    auto LightBuffer = renderer->CreateResource("Light Buffer", gpu->GetLightBuffer());
    auto LightAliasTableBuffer = renderer->CreateResource("Light Alias Table Buffer", gpu->GetLightAliasTableBuffer());
    auto SobolMatricesBuffer = renderer->CreateResource("Sobol Matrices Buffer", gpu->GetSobolMatricesBuffer());
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
    const RHIResourceFormat postprocessFormat = cfg.enableHDR
        ? RHIResourceFormat::A2B10G10R10Unorm
        : RHIResourceFormat::R8G8B8A8Unorm;
    auto PostprocessBuffer = renderer->CreateResource(
        "Postprocess",
        RHITextureDesc{.usage = RHITextureUsageBits::RenderTarget |
                                  RHITextureUsageBits::SampledImage |
                                  RHITextureUsageBits::TransferSource,
                       .extent = {w, h, 1},
                       .format = postprocessFormat});
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
    outHandles.sdrRT = PostprocessBuffer;
    outHandles.pickBuffer = PickResultBuffer;

    ResourceHandle EnvMapSampler = renderer->CreateSampler({
        .filter = {RHIDeviceSampler::SamplerDesc::Filter::Linear, RHIDeviceSampler::SamplerDesc::Filter::Linear},
    });
    auto LUTSampler = renderer->CreateSampler({.addressMode = {
                                                   .u = RHIDeviceSampler::SamplerDesc::AddressMode::ClampToEdge,
                                                   .v = RHIDeviceSampler::SamplerDesc::AddressMode::ClampToEdge,
                                                   .w = RHIDeviceSampler::SamplerDesc::AddressMode::ClampToEdge,
                                               }});
    if (hasTLAS)
    {
        renderer->CreatePass(
            "Trace", RHIDeviceQueueType::Graphics, 0u,
            [=](PassHandle self, Renderer* r)
        {
            using RTHitGroupType = RHIPipelineState::PipelineStateDesc::RayTracingHitGroupType;
            r->BindBufferUniform(self, GlobalUBO, RHIPipelineStageBits::RayTracingShader, "globalParams");
            if (hasTLAS)
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
            r->BindBufferStorageRead(self, InstanceBuffer, RHIPipelineStageBits::ComputeShader, "instances");
            r->BindBufferStorageRead(self, MaterialBuffer, RHIPipelineStageBits::ComputeShader, "materials");
            r->BindBufferStorageRead(self, LightBuffer, RHIPipelineStageBits::ComputeShader, "lights");
            r->BindBufferStorageRead(self, LightAliasTableBuffer, RHIPipelineStageBits::ComputeShader, "lightAliasTable");
            r->BindBufferStorageRead(self, SobolMatricesBuffer, RHIPipelineStageBits::ComputeShader, "sobolMatrices");
            r->BindTextureSampler(self, TexSampler, "textureSampler");
            r->BindTextureSampler(self, LUTSampler, "lutSampler");
            // Accumulation UAVs (Welford online mean)
            r->BindTextureUAV(self, Diffuse, "diffuse", RHIPipelineStageBits::RayTracingShader,
                              RHITextureViewDesc{.format = RHIResourceFormat::R32G32B32A32SignedFloat,
                                                 .range = RHITextureSubresourceRange::Create()});
            r->BindTextureUAV(self, Specular, "specular", RHIPipelineStageBits::RayTracingShader,
                              RHITextureViewDesc{.format = RHIResourceFormat::R32G32B32A32SignedFloat,
                                                 .range = RHITextureSubresourceRange::Create()});
            r->BindTextureUAV(self, PickIDBuffer, "pickIDBuffer", RHIPipelineStageBits::RayTracingShader,
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
            bool canTrace = (!renderPaused || !*renderPaused) &&
                (!scene.rendererRebuildRequested || !*scene.rendererRebuildRequested);
            if (canTrace)
            {
                uint32_t tileSide = PTDispatchTileSide(*scene.gsGlobals);
                cmd->TraceRays((w - 1u) / tileSide + 1u, (h - 1u) / tileSide + 1u, 1);
            }
        });
    }
    else
    {
        auto CreateClearRGBA = [=](StringView name, ResourceHandle texture)
        {
            renderer->CreatePass(
                name, RHIDeviceQueueType::Compute, 0u,
                [=](PassHandle self, Renderer* r)
                {
                    r->BindTextureUAV(self, texture, "texture", RHIPipelineStageBits::ComputeShader,
                                      {.format = RHIResourceFormat::R32G32B32A32SignedFloat,
                                       .range = RHITextureSubresourceRange::Create(RHITextureAspectFlagBits::Color)});
                    r->BindShader(self, RHIShaderStageBits::Compute, "main", PathsResolve("Data/Shaders/CSClearBuffer.spv"));
                    r->BindPushConstant(self, RHIShaderStageBits::Compute, 0, sizeof(CSClearBufferData));
                },
                [=](PassHandle self, Renderer* r, RHICommandList* cmd)
                {
                    CSClearBufferData cdata{float4{}, w, h};
                    r->CmdSetPipeline(self, cmd);
                    r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Compute, 0, cdata);
                    r->CmdDispatch(self, cmd, {cdata.w, cdata.h, 1});
                });
        };
        CreateClearRGBA("Clear Diffuse", Diffuse);
        CreateClearRGBA("Clear Specular", Specular);
        renderer->CreatePass(
            "Clear Pick ID", RHIDeviceQueueType::Compute, 0u,
            [=](PassHandle self, Renderer* r)
            {
                r->BindTextureUAV(self, PickIDBuffer, "texture", RHIPipelineStageBits::ComputeShader,
                                  {.format = RHIResourceFormat::R32Uint,
                                   .range = RHITextureSubresourceRange::Create(RHITextureAspectFlagBits::Color)});
                r->BindShader(self, RHIShaderStageBits::Compute, "main", PathsResolve("Data/Shaders/ECSPickIDClear.spv"));
                r->BindPushConstant(self, RHIShaderStageBits::Compute, 0, sizeof(CSClearBufferData));
            },
            [=](PassHandle self, Renderer* r, RHICommandList* cmd)
            {
                CSClearBufferData cdata{float4{}, w, h};
                r->CmdSetPipeline(self, cmd);
                r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Compute, 0, cdata);
                r->CmdDispatch(self, cmd, {cdata.w, cdata.h, 1});
            });
    }

    createPSFullscreenPassRTV(
        renderer, "Postprocess", PostprocessBuffer,
        RHITextureViewDesc{.format = postprocessFormat,
                           .range = RHITextureSubresourceRange::Create()},
        {w, h},
        [=](PassHandle self, Renderer* r)
        {
            r->BindShader(self, RHIShaderStageBits::Fragment, "fragMain",
                          PathsResolve("Data/Shaders/EPSPostprocessPT.spv"), AsBytes(AsSpan(cfg.viewFlags)));
            auto bindSRV = [&](ResourceHandle h, const char* name)
            {
                r->BindTextureSRV(self, h, name, RHIPipelineStageBits::FragmentShader,
                                  RHITextureViewDesc{.format = RHIResourceFormat::R32G32B32A32SignedFloat,
                                                     .range = RHITextureSubresourceRange::Create()});
            };
            bindSRV(Diffuse, "diffuseTex");
            bindSRV(Specular, "specularTex");
            r->BindBufferUniform(self, GlobalUBO, RHIPipelineStageBits::FragmentShader, "globalParams");
            r->BindDescriptorSet(self, "textures3D", gpu->GetTexture3DPool()->GetDescriptorSetLayout());
            r->BindTextureSampler(self, LUTSampler, "lutSampler");
        },
        [=](PassHandle self, Renderer* r, RHICommandList* cmd)
        {
            r->CmdBindDescriptorSet(self, cmd, "textures3D", gpu->GetTexture3DPool()->GetDescriptorSet());
        });

    createPSFullscreenPass(
        renderer, "Blit Image",
        [=](PassHandle self, Renderer* r)
        {
            r->BindShader(self, RHIShaderStageBits::Fragment, "fragMain", PathsResolve("Data/Shaders/EPSBlitPT.spv"));
            r->BindTextureSRV(self, PostprocessBuffer, "displayImage", RHIPipelineStageBits::FragmentShader,
                              RHITextureViewDesc{.format = postprocessFormat,
                                                 .range = RHITextureSubresourceRange::Create()});
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