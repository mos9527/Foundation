#include <RenderUtils/CSClearBuffer.hpp>
#include <RenderUtils/CSMipGeneration.hpp>
#include <RenderUtils/PSFullscreen.hpp>
#include <algorithm>
#include "../Paths.hpp"
#include "Render.hpp"
using namespace RenderUtils;
#pragma pack(push, 1)
struct MeshletTaskDispatch // VkDrawMeshTasksIndirectCommandEXT
{
    uint32_t groupCountX;
    uint32_t groupCountY;
    uint32_t groupCountZ;
};
struct MeshletTaskWork
{
    uint32_t instanceID; // Absolute
    // Task shader can *only* dispatch zero or one meshlet per work thread.
    // Hence, batching is required here. Whereas in Mesh Shader multiple verts/tris
    // can be processed per thread.
    uint32_t firstMeshlet;
    uint32_t numMeshlets;
};
#pragma pack(pop)
constexpr size_t kMeshWorkGroupSize = 64;
constexpr size_t kMaxMeshletCount = 1e6;
constexpr size_t kMaxMeshletTaskWorkCount = kMaxMeshletCount / kMeshWorkGroupSize;
void BuildIdleRenderGraph(FContext* context, float const* timeSeconds)
{
    auto* renderer = context->renderer;
    CHECK(renderer);
    createPSFullscreenPass(
        renderer, "Idle",
        [=](PassHandle self, Renderer* r)
        {
            r->BindShader(self, RHIShaderStageBits::Fragment, "fragMain",
                          Paths::Resolve("data/shaders/EPSIdle.spv"));
            r->BindPushConstant(self, RHIShaderStageBits::Fragment, 0, sizeof(float2));
        },
        [=](PassHandle self, Renderer* r, RHICommandList* cmd)
        {
            r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Fragment, 0,
                                  float2{*timeSeconds, (float)r->GetSwapchainExtent().x / r->GetSwapchainExtent().y });
        });
}

void BuildRasterRenderGraph(FContext* context, RendererConfig cfg, RendererScene scene, RHIExtent2D renderExtent,
                            RendererHandles& outHandles)
{
    CHECK(context->device->GetCapabilities().meshShaders);
    auto* renderer = context->renderer;
    CHECK(renderer);
    auto* gpu = context->gpuScene;
    /* UBO for everyone */
    auto GlobalUBO = renderer->CreateResource(
        "Global UBO",
        RHIBufferDesc{.usage = RHIBufferUsageBits::TransferDestination | RHIBufferUsageBits::UniformBuffer,
                      .size = sizeof(UBO)});
    /* Instance and Primitive buffers */
    auto TLAS = renderer->CreateResource("Scene TLAS", gpu->GetTLAS());
    auto PrimitiveBuffer = renderer->CreateResource("Primitive Buffer", gpu->GetPrimitiveBuffer());

    auto InstanceBuffer = renderer->CreateResource("Instance Buffer", gpu->GetInstanceBuffer());
    auto MaterialBuffer = renderer->CreateResource("Material Buffer", gpu->GetMaterialBuffer());
    auto LightBuffer = renderer->CreateResource("Light Buffer", gpu->GetLightBuffer());
    /* Indirect Task Buffers */
    using enum RHIBufferUsageBits;
    auto IndirectTasks =
        renderer->CreateResource("Indirect Meshlet Cull CS Buffer", // Instance IDs -> Task Work
                                 RHIBufferDesc{.usage = IndirectBuffer | StorageBuffer,
                                               .size = sizeof(MeshletTaskWork) * kMaxMeshletTaskWorkCount});
    auto IndirectTaskCounter = renderer->CreateResource(
        "Indirect Meshlet Cull CS Counter",
        RHIBufferDesc{.usage = IndirectBuffer | StorageBuffer | TransferDestination, .size = sizeof(int)});
    // vvv Used to launch the actual culling CS
    auto IndirectTaskDispatch =
        renderer->CreateResource("Indirect Meshlet Cull CS Dispatch",
                                 RHIBufferDesc{.usage = IndirectBuffer | StorageBuffer | TransferDestination,
                                               .size = sizeof(MeshletTaskDispatch)});
    auto IndirectMeshlets =
        renderer->CreateResource("Indirect Draw MS Buffer", // Task Work -> UINT2 Meshlet IDs [x: Meshlet, y: Instance]
                                 RHIBufferDesc{.usage = IndirectBuffer | StorageBuffer | TransferDestination,
                                               .size = 2 * sizeof(int) * kMaxMeshletCount});
    auto IndirectMeshletCounter = renderer->CreateResource(
        "Indirect Draw MS Counter",
        RHIBufferDesc{.usage = IndirectBuffer | StorageBuffer | TransferDestination, .size = sizeof(int)});
    // vvv Used to launch the actual mesh shader draws
    auto IndirectMeshletDispatch =
        renderer->CreateResource("Indirect Draw MS Dispatch",
                                 RHIBufferDesc{.usage = IndirectBuffer | StorageBuffer | TransferDestination,
                                               .size = sizeof(MeshletTaskDispatch)});
    // We pack meshlet visibility in uint32 bitmaps
    auto Visibility =
        renderer->CreateResource("Visibility Buffer",
                                 RHIBufferDesc{.usage = StorageBuffer | TransferDestination,
                                               .size = AlignUp(kMaxMeshletCount, 32) / 32 * sizeof(uint32_t)});
    
    auto GGXlutE = renderer->CreateResource("GGX LUT E", gpu->GetGGXlutE());
    RHITexture* viewLutTexture = context->enableHDR ? gpu->GetViewLutHdr() : gpu->GetViewLutSdr();
    RHIResourceFormat viewLutFormat = viewLutTexture->mDesc.format;
    auto ViewLut = renderer->CreateResource(context->enableHDR ? "View LUT HDR" : "View LUT SDR", viewLutTexture);
    ResourceHandle EnvMapTex;
    if (gpu->GetEnvMap()) {
        EnvMapTex = renderer->CreateResource("Env Map", gpu->GetEnvMap());
    } else {
        EnvMapTex = renderer->CreateResource("Env Map Fallback", RHITextureDesc{
            .usage = RHITextureUsageBits::SampledImage | RHITextureUsageBits::TransferDestination,
            .extent = {1, 1, 1},
            .format = RHIResourceFormat::R8G8B8A8Unorm});
    }

    // NOTE: Lambda captures
    // NONE of the handle values outlive the renderer. Therefore, ALWAYS capture by value.
    renderer->CreatePass(
        "UBO Update & Init", RHIDeviceQueueType::Graphics, 0u,
        [=](PassHandle self, Renderer* r)
        {
            r->BindBufferCopyDst(self, GlobalUBO);
            r->BindBufferCopyDst(self, IndirectTaskCounter);
        },
        [=](PassHandle, Renderer* r, RHICommandList* cmd)
        {
            auto* ubo = r->DerefResource(GlobalUBO).Get<RHIBuffer*>();
            auto* counter = r->DerefResource(IndirectTaskCounter).Get<RHIBuffer*>();
            // Fill, Update are considered Transfer operations
            // and would require proper barriers - which are automatically handled
            // by the Renderer *inter* passes.
            // Note that usage before a Dispatch, etc, may be valid but is still a ROW hazard.
            // TODO: Document these.
            cmd->UpdateBuffer(ubo, 0, AsBytes(AsSpan(*scene.gsGlobals)));
            cmd->FillBuffer(counter, 0u);
        });
    bool kDebugViewUnlit = cfg.viewFlags & (kViewBaseColor | kViewNormal | kViewMaterialID | kViewMeshlet);
    // Raytracing
    if (cfg.viewFlags & kEnableRasterRTShadows && !kDebugViewUnlit)
    {
        renderer->CreatePass(
            "TLAS Update", RHIDeviceQueueType::Graphics, 0u, [=](PassHandle self, Renderer* r)
            { r->BindAccelerationStructureWrite(self, TLAS); }, [=](PassHandle, Renderer* r, RHICommandList* cmd)
            {
                if (scene.gsInstances->empty() && scene.gsLights->empty())
                    return;
                gpu->BuildTLAS(cmd, *scene.gsInstances, *scene.gsBLASes, {}, {}, *scene.gsLights, true);
            });
    }
    renderer->CreatePass(
        "Indirect Meshlet Cull Clear", RHIDeviceQueueType::Graphics, 0u,
        [=](PassHandle self, Renderer* r)
        {
            r->BindBufferCopyDst(self, IndirectTaskDispatch);
        },
        [=](PassHandle, Renderer* r, RHICommandList* cmd)
        {
            auto* taskDispatch = r->DerefResource(IndirectTaskDispatch).Get<RHIBuffer*>();
            cmd->FillBuffer(taskDispatch, 0u);
        });
    renderer->CreatePass(
        "Indirect Meshlet Cull Generation", RHIDeviceQueueType::Graphics, 0u,
        [=](PassHandle self, Renderer* r)
        {
            r->BindShader(self, RHIShaderStageBits::Compute, "main", Paths::Resolve("data/shaders/ECSCullInstances.spv"));
            r->BindBufferUniform(self, GlobalUBO, RHIPipelineStageBits::ComputeShader, "globalParams");
            r->BindBufferStorageRead(self, InstanceBuffer, RHIPipelineStageBits::ComputeShader, "instances");
            r->BindBufferStorageRead(self, PrimitiveBuffer, RHIPipelineStageBits::ComputeShader, "primitive");
            r->BindBufferUnordered(self, IndirectTasks, RHIPipelineStageBits::ComputeShader, "outTasks");
            r->BindBufferUnordered(self, IndirectTaskCounter, RHIPipelineStageBits::ComputeShader, "outTasksCounter");
            r->BindBufferUnordered(self, IndirectTaskDispatch, RHIPipelineStageBits::ComputeShader, "outTasksDispatch");
        },
        [=](PassHandle self, Renderer* r, RHICommandList* cmd)
        {
            r->CmdSetPipeline(self, cmd);
            // TODO: This limits us to 65536 instances
            r->CmdDispatch(self, cmd, {scene.gsGlobals->numInstances, 1, 1});
        });
    /* Meshlet Drawing */
    uint32_t w = std::max(renderExtent.x, 16u);
    uint32_t h = std::max(renderExtent.y, 16u);
    auto ZBuffer = renderer->CreateResource(
        "ZBuffer",
        RHITextureDesc{.usage = RHITextureUsageBits::DepthStencil | RHITextureUsageBits::SampledImage,
                       .extent = {w, h, 1},
                       .format = RHIResourceFormat::D32SignedFloat});
    // Half-res as we start downsampling from mip 1
    uint32_t HIZWidth = 1u << glm::log2(w / 2), HIZHeight = 1u << glm::log2(h / 2);
    if (HIZWidth * 2 < w)
        HIZWidth *= 2;
    if (HIZHeight * 2 < h)
        HIZHeight *= 2;
    HIZWidth /= 2, HIZHeight /= 2;
    const uint32_t HIZMips = glm::log2(std::max(HIZWidth, HIZHeight)) + 1u;
    scene.gsGlobals->hizWidth = HIZWidth, scene.gsGlobals->hizHeight = HIZHeight, scene.gsGlobals->hizLevels = HIZMips;
    RHIDeviceSampler::SamplerDesc HIZSamplerDesc{
        .addressMode = {.u = RHIDeviceSampler::SamplerDesc::AddressMode::ClampToEdge,
                        .v = RHIDeviceSampler::SamplerDesc::AddressMode::ClampToEdge,
                        .w = RHIDeviceSampler::SamplerDesc::AddressMode::ClampToEdge},
        .mipmap = {.mipmapMode = RHIDeviceSampler::SamplerDesc::Mipmap::Nearest},
        .reduction = RHIDeviceSampler::SamplerDesc::Reduction::Min};
    auto HIZSampler = renderer->CreateSampler(HIZSamplerDesc);
    auto TexSampler = renderer->CreateSampler({});
    auto HIZ = renderer->CreateResource(
        "HIZ",
        RHITextureDesc{.usage = RHITextureUsageBits::StorageImage | RHITextureUsageBits::SampledImage,
                       .extent = {HIZWidth, HIZHeight, 1},
                       .format = RHIResourceFormat::R32SignedFloat,
                       .mipLevels = HIZMips});
    auto OverdrawBuffer = renderer->CreateResource("Overdraw Buffer",
                                                   RHITextureDesc{.usage = RHITextureUsageBits::RenderTarget |
                                                                      RHITextureUsageBits::StorageImage |
                                                                      RHITextureUsageBits::SampledImage,
                                                                  .extent = {w, h, 1},
                                                                  .format = RHIResourceFormat::R32Uint});
    auto GBufferRT0 = renderer->CreateResource("GBuffer 0",
                                               RHITextureDesc{.usage = RHITextureUsageBits::RenderTarget |
                                                                  RHITextureUsageBits::StorageImage |
                                                                  RHITextureUsageBits::SampledImage,
                                                              .extent = {w, h, 1},
                                                              .format = RHIResourceFormat::R8G8B8A8Unorm});
    auto GBufferRT1 = renderer->CreateResource("GBuffer 1",
                                               RHITextureDesc{.usage = RHITextureUsageBits::RenderTarget |
                                                                  RHITextureUsageBits::StorageImage |
                                                                  RHITextureUsageBits::SampledImage,
                                                              .extent = {w, h, 1},
                                                              .format = RHIResourceFormat::R8G8B8A8Unorm});
    auto GBufferRT2 = renderer->CreateResource("GBuffer 2",
                                               RHITextureDesc{.usage = RHITextureUsageBits::RenderTarget |
                                                                  RHITextureUsageBits::StorageImage |
                                                                  RHITextureUsageBits::SampledImage,
                                                              .extent = {w, h, 1},
                                                              .format = RHIResourceFormat::R16G16B16A16SignedFloat});
    // Instance ID map: R32_UINT, one uint per pixel storing the absolute instance index.
    // ~0u means "no object" (cleared each frame).
    auto PickIDBuffer = renderer->CreateResource("Pick ID Buffer",
                                                RHITextureDesc{.usage = RHITextureUsageBits::RenderTarget |
                                                                   RHITextureUsageBits::SampledImage,
                                                               .extent = {w, h, 1},
                                                               .format = RHIResourceFormat::R32Uint});
    // 4-byte persistently-mapped readback buffer: Blit PS writes the picked instanceID here.
    // ~0u = no object / no pending pick.
    auto PickResultBuffer = renderer->CreateResource("Pick Result Buffer",
        RHIBufferDesc{.resource = {.heap = RHIDeviceHeapType::Readback,
                                   .hostAccess = RHIResourceHostAccess::ReadWrite,
                                   .coherent = true},
                      .usage = RHIBufferUsageBits::StorageBuffer,
                      .size = sizeof(uint32_t)});
    auto ReduceBuffer = renderer->CreateResource(
        "Reduced Values", RHIBufferDesc{.usage = StorageBuffer | TransferDestination, .size = sizeof(uint32_t) * 256});
    if (cfg.viewFlags & kViewOverdraw)
    {
        renderer->CreatePass(
            "Clear Overdraw+Reduce Buffer", RHIDeviceQueueType::Compute, 0u,
            [=](PassHandle self, Renderer* r)
            {
                r->BindTextureUAV(self, OverdrawBuffer, "texture", RHIPipelineStageBits::ComputeShader,
                                  {.format = RHIResourceFormat::R32Uint,
                                   .range = RHITextureSubresourceRange::Create(RHITextureAspectFlagBits::Color)});
                r->BindShader(self, RHIShaderStageBits::Compute, "main", Paths::Resolve("data/shaders/ECSOverdrawClear.spv"));
                r->BindPushConstant(self, RHIShaderStageBits::Compute, 0, sizeof(CSClearBufferData));
                r->BindBufferCopyDst(self, ReduceBuffer);
            },
            [=](PassHandle self, Renderer* r, RHICommandList* cmd)
            {
                auto* reduceBuffer = r->DerefResource(ReduceBuffer).Get<RHIBuffer*>();
                cmd->FillBuffer(reduceBuffer, 0u);
                RHIExtent2D wh{w, h};
                CSClearBufferData cdata{float4{}, wh.x, wh.y};
                r->CmdSetPipeline(self, cmd);
                r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Compute, 0, cdata);
                r->CmdDispatch(self, cmd, {cdata.w, cdata.h, 1});
            });
    }
    /* Main Pass */
    {
        auto AddCullPass = [=](bool early)
        {
            // Instance -> Meshlet Tasks
            // Note that we don't actually use Task Shaders - for inexplicable reasons described
            // in ECSCullMeshlets.
            renderer->CreatePass(
                early ? "Indirect Meshlet Cull Clear [Stage 1]" : "Indirect Meshlet Cull Clear [Stage 2]",
                RHIDeviceQueueType::Graphics, 0u,
                [=](PassHandle self, Renderer* r)
                {
                    r->BindBufferCopyDst(self, IndirectMeshletCounter);
                    r->BindBufferCopyDst(self, IndirectMeshletDispatch);
                },
                [=](PassHandle, Renderer* r, RHICommandList* cmd)
                {
                    auto* msCounter = r->DerefResource(IndirectMeshletCounter).Get<RHIBuffer*>();
                    auto* msDispatches = r->DerefResource(IndirectMeshletDispatch).Get<RHIBuffer*>();
                    cmd->FillBuffer(msCounter, 0u);
                    cmd->FillBuffer(msDispatches, 0u);
                });
            renderer->CreatePass(
                early ? "Indirect Meshlet Cull Dispatch [Stage 1]" : "Indirect Meshlet Cull Dispatch [Stage 2]",
                RHIDeviceQueueType::Graphics, 0u,
                [=](PassHandle self, Renderer* r)
                {
                    int flags = cfg.cullFlags;
                    if (early)
                        flags |= kCullStageEarly;
                    else
                        flags |= kCullStageLate;
                    r->BindShader(self, RHIShaderStageBits::Compute, "main", Paths::Resolve("data/shaders/ECSCullMeshlets.spv"),
                                  AsBytes(AsSpan(flags)));
                    r->BindBufferUniform(self, GlobalUBO, RHIPipelineStageBits::ComputeShader, "globalParams");
                    r->BindBufferIndirectRead(self, IndirectTaskDispatch);
                    r->BindBufferStorageRead(self, InstanceBuffer, RHIPipelineStageBits::ComputeShader, "instances");
                    r->BindBufferStorageRead(self, PrimitiveBuffer, RHIPipelineStageBits::ComputeShader, "primitive");
                    r->BindBufferUnordered(self, Visibility, RHIPipelineStageBits::ComputeShader, "visibility");
                    r->BindBufferUnordered(self, IndirectTaskCounter, RHIPipelineStageBits::ComputeShader, "inTasksCounter");
                    r->BindBufferUnordered(self, IndirectTasks, RHIPipelineStageBits::ComputeShader, "inTasks");
                    r->BindBufferUnordered(self, IndirectMeshletCounter, RHIPipelineStageBits::ComputeShader, "outMeshletCounter");
                    r->BindBufferUnordered(self, IndirectMeshlets, RHIPipelineStageBits::ComputeShader, "outMeshletIndices");
                    r->BindBufferUnordered(self, IndirectMeshletDispatch, RHIPipelineStageBits::ComputeShader, "outMeshletDispatches");
                    r->BindTextureSampler(self, HIZSampler, "hizSampler");
                    r->BindTextureSRV(self, HIZ, "hiz", RHIPipelineStageBits::ComputeShader,
                                      RHITextureViewDesc{.format = RHIResourceFormat::R32SignedFloat,
                                                         .range = RHITextureSubresourceRange::Create(
                                                             RHITextureAspectFlagBits::Color, 0, HIZMips)});
                },
                [=](PassHandle self, Renderer* r, RHICommandList* cmd)
                {
                    r->CmdSetPipeline(self, cmd);
                    auto* dispatchBuffer = r->DerefResource(IndirectTaskDispatch).Get<RHIBuffer*>();
                    cmd->DispatchIndirect(dispatchBuffer, 0);
                });
        };
        auto AddMainPass = [=](bool early)
        {
            renderer->CreatePass(
                early ? "Main [Stage 1]" : "Main [Stage 2]", RHIDeviceQueueType::Graphics, 0u,
                [=](PassHandle self, Renderer* r)
                {
                    // This is what we could've had. Instead of AddCullPass if it actually works on all platforms.
                    // It should. But it doesn't.
                    // r->BindShader(self, RHIShaderStageBits::Task, "main", Paths::Resolve("data/shaders/ETSMeshletCull.spv"),
                    // AsBytes(AsSpan(TSFlags)));
                    r->BindShader(self, RHIShaderStageBits::Mesh, "main", Paths::Resolve("data/shaders/EMSBasic.spv"));
                    r->BindShader(self, RHIShaderStageBits::Fragment, "main", Paths::Resolve("data/shaders/EPSGBuffer.spv"),
                                  AsBytes(AsSpan(cfg.viewFlags)));
                    r->BindBufferUniform(self, GlobalUBO, RHIPipelineStageBits::AllGraphics, "globalParams");
                    r->BindBufferStorageRead(self, PrimitiveBuffer, RHIPipelineStageBits::AllGraphics, "primitive");
                    r->BindBufferStorageRead(self, InstanceBuffer, RHIPipelineStageBits::AllGraphics, "instances");
                    r->BindBufferStorageRead(self, MaterialBuffer, RHIPipelineStageBits::AllGraphics, "materials");
                    r->BindBufferStorageRead(self, IndirectMeshletCounter, RHIPipelineStageBits::AllGraphics, "inMeshletCounter");
                    r->BindBufferStorageRead(self, IndirectMeshlets, RHIPipelineStageBits::AllGraphics, "inMeshletIndices");
                    r->BindTextureSRV(self, GGXlutE, "ggxLutE", RHIPipelineStageBits::AllGraphics,
                                      RHITextureViewDesc{.format = RHIResourceFormat::R32G32SignedFloat,
                                                         .range = RHITextureSubresourceRange::Create()});
                    r->BindTextureRTV(self, GBufferRT0,
                                      {.format = RHIResourceFormat::R8G8B8A8Unorm,
                                       .range = RHITextureSubresourceRange::Create(RHITextureAspectFlagBits::Color)});
                    r->BindTextureRTV(self, GBufferRT1,
                                      {.format = RHIResourceFormat::R8G8B8A8Unorm,
                                       .range = RHITextureSubresourceRange::Create(RHITextureAspectFlagBits::Color)});
                    r->BindTextureRTV(self, GBufferRT2,
                                      {.format = RHIResourceFormat::R16G16B16A16SignedFloat,
                                       .range = RHITextureSubresourceRange::Create(RHITextureAspectFlagBits::Color)});
                    r->BindTextureRTV(self, PickIDBuffer,
                                      {.format = RHIResourceFormat::R32Uint,
                                       .range = RHITextureSubresourceRange::Create(RHITextureAspectFlagBits::Color)});
                    r->BindTextureUAV(self, OverdrawBuffer, "overdraw", RHIPipelineStageBits::FragmentShader,
                                      {.format = RHIResourceFormat::R32Uint,
                                       .range = RHITextureSubresourceRange::Create(RHITextureAspectFlagBits::Color)});
                    r->BindTextureDSV(self, ZBuffer,
                                      {.format = RHIResourceFormat::D32SignedFloat,
                                       .range = RHITextureSubresourceRange::Create(RHITextureAspectFlagBits::Depth)});
                    r->BindBufferIndirectRead(self, IndirectMeshletDispatch);
                    r->BindTextureSampler(self, TexSampler, "textureSampler");
                    r->BindDescriptorSet(self, "textures",
                                         context->gpuScene->GetTexturePool()->GetDescriptorSetLayout());
                },
                [=](PassHandle self, Renderer* r, RHICommandList* cmd)
                {
                    RHIExtent2D wh{w, h};
                    if (early)
                        r->CmdBeginGraphics(self, cmd, wh,
                                            {{{RHIAttachmentLoadOp::Clear},
                                              {RHIAttachmentLoadOp::Clear},
                                              {RHIAttachmentLoadOp::Clear},
                                              {RHIAttachmentLoadOp::Clear}}},
                                            {RHIAttachmentLoadOp::Clear, {0.0f, 0}});
                    else // Don't clear depth in stage 2.
                        r->CmdBeginGraphics(self, cmd, wh,
                                            {{{RHIAttachmentLoadOp::Load},
                                              {RHIAttachmentLoadOp::Load},
                                              {RHIAttachmentLoadOp::Load},
                                              {RHIAttachmentLoadOp::Load}}},
                                            {RHIAttachmentLoadOp::Load});
                    r->CmdSetPipeline(self, cmd);
                    cmd->SetViewport(0, 0, w, h, 0, 1, true).SetScissor(0, 0, w, h);
                    r->CmdBindDescriptorSet(self, cmd, "textures",
                                            context->gpuScene->GetTexturePool()->GetDescriptorSet());
                    auto* dispatchBuffer = r->DerefResource(IndirectMeshletDispatch).Get<RHIBuffer*>();
                    cmd->DrawMeshTasksIndirect(dispatchBuffer, 0, 1, sizeof(MeshletTaskDispatch));
                    cmd->EndGraphics();
                });
            if (early && cfg.cullFlags & kCullOcclusion)
            {
                // Don't bother going Async for now - this is used immediately after and there's no other work
                // to overlap with.
                // TODO: Single pass currently present higher register pressure than expected (72 VGPRs?)
                //       Figure out where I messed up.                
                if (true)
                    createCSMipGenerationSinglePass(renderer, "Early HiZ Mip Gen", RHIDeviceQueueType::Graphics, ZBuffer,
                                                    HIZ, RHIResourceFormat::D32SignedFloat,
                                                    RHIResourceFormat::R32SignedFloat, RHITextureAspectFlagBits::Depth,
                                                    RHITextureAspectFlagBits::Color, HIZSampler, HIZMips, 1,
                                                    HIZSamplerDesc.reduction);
                else
                    createCSMipGenerationPasses(renderer, "Early HiZ Mip Gen", RHIDeviceQueueType::Graphics, ZBuffer, HIZ,
                                                RHIResourceFormat::D32SignedFloat, RHIResourceFormat::R32SignedFloat,
                                                RHITextureAspectFlagBits::Depth, RHITextureAspectFlagBits::Color,
                                                HIZSampler, HIZMips);
            }
        };
        AddCullPass(true), AddMainPass(true);
        if (cfg.cullFlags & kCullOcclusion)
            AddCullPass(false), AddMainPass(false);
    }
    if (cfg.viewFlags & kViewOverdraw)
    {
        renderer->CreatePass(
            "Overdraw CS Reduce", RHIDeviceQueueType::Compute, 0u,
            [=](PassHandle self, Renderer* r)
            {
                r->BindShader(self, RHIShaderStageBits::Compute, "main", Paths::Resolve("data/shaders/ECSOverdrawReduce.spv"));
                r->BindTextureSRV(self, OverdrawBuffer, "texture", RHIPipelineStageBits::ComputeShader,
                                  RHITextureViewDesc{.format = RHIResourceFormat::R32Uint,
                                                     .range = RHITextureSubresourceRange::Create()});
                r->BindBufferUnordered(self, ReduceBuffer, RHIPipelineStageBits::ComputeShader, "globalMax");
                r->BindPushConstant(self, RHIShaderStageBits::Compute, 0, sizeof(RHIExtent2D));
            },
            [=](PassHandle self, Renderer* r, RHICommandList* cmd)
            {
                RHIExtent2D wh{w, h};
                r->CmdSetPipeline(self, cmd);
                r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Compute, 0, wh);
                r->CmdDispatch(self, cmd, {wh.x, wh.y, 1});
            });
    }
    auto LightingBuffer = renderer->CreateResource(
        "Lighting",
        RHITextureDesc{.usage = RHITextureUsageBits::StorageImage |
                                  RHITextureUsageBits::SampledImage |
                                  RHITextureUsageBits::TransferSource,
                       .extent = {w, h, 1},
                       .format = RHIResourceFormat::R32G32B32A32SignedFloat});
    const RHIResourceFormat postprocessFormat = context->enableHDR
        ? RHIResourceFormat::A2B10G10R10Unorm
        : RHIResourceFormat::R8G8B8A8Unorm;
    auto PostprocessBuffer = renderer->CreateResource(
        "Postprocess",
        RHITextureDesc{.usage = RHITextureUsageBits::RenderTarget |
                                  RHITextureUsageBits::SampledImage |
                                  RHITextureUsageBits::TransferSource,
                       .extent = {w, h, 1},
                       .format = postprocessFormat});

    auto LUTSampler = renderer->CreateSampler({
    .addressMode = {
        .u = RHIDeviceSampler::SamplerDesc::AddressMode::ClampToEdge,
        .v = RHIDeviceSampler::SamplerDesc::AddressMode::ClampToEdge,
        .w = RHIDeviceSampler::SamplerDesc::AddressMode::ClampToEdge,
    }
});
    auto EnvMapSampler = renderer->CreateSampler({
        .filter = {RHIDeviceSampler::SamplerDesc::Filter::Linear,
                   RHIDeviceSampler::SamplerDesc::Filter::Linear},
    });
    renderer->CreatePass(
        "Lighting", RHIDeviceQueueType::Graphics, 0u,
        [=](PassHandle self, Renderer* r)
        {
            r->BindShader(self, RHIShaderStageBits::Compute, "main", Paths::Resolve("data/shaders/ECSLighting.spv"),
                          AsBytes(AsSpan(cfg.viewFlags)));
            r->BindBufferUniform(self, GlobalUBO, RHIPipelineStageBits::ComputeShader, "globalParams");
            r->BindTextureSRV(self, GBufferRT0, "RT0", RHIPipelineStageBits::ComputeShader,
                              RHITextureViewDesc{.format = RHIResourceFormat::R8G8B8A8Unorm,
                                                 .range = RHITextureSubresourceRange::Create()});
            r->BindTextureSRV(self, GBufferRT1, "RT1", RHIPipelineStageBits::ComputeShader,
                              RHITextureViewDesc{.format = RHIResourceFormat::R8G8B8A8Unorm,
                                                 .range = RHITextureSubresourceRange::Create()});
            r->BindTextureSRV(self, GBufferRT2, "RT2", RHIPipelineStageBits::ComputeShader,
                              RHITextureViewDesc{.format = RHIResourceFormat::R16G16B16A16SignedFloat,
                                                 .range = RHITextureSubresourceRange::Create()});
            r->BindTextureSRV(
                self, ZBuffer, "depth", RHIPipelineStageBits::ComputeShader,
                RHITextureViewDesc{.format = RHIResourceFormat::D32SignedFloat,
                                   .range = RHITextureSubresourceRange::Create(RHITextureAspectFlagBits::Depth)});
            r->BindAccelerationStructureSRV(self, TLAS, RHIPipelineStageBits::ComputeShader, "AS");
            r->BindBufferStorageRead(self, LightBuffer, RHIPipelineStageBits::ComputeShader, "lights");
            r->BindTextureSampler(self, LUTSampler, "lutSampler");
            r->BindTextureSRV(self, GGXlutE, "ggxLutE", RHIPipelineStageBits::ComputeShader,
                  RHITextureViewDesc{.format = RHIResourceFormat::R32G32SignedFloat,
                                     .range = RHITextureSubresourceRange::Create()});
            r->BindTextureSRV(self, EnvMapTex, "envMapTexture", RHIPipelineStageBits::RayTracingShader,
                              RHITextureViewDesc{.format = gpu->GetEnvMap()
                                                     ? RHIResourceFormat::R32G32B32A32SignedFloat
                                                     : RHIResourceFormat::R8G8B8A8Unorm,
                                                 .range = RHITextureSubresourceRange::Create()});
            r->BindTextureSampler(self, EnvMapSampler, "envMapSampler");
            r->BindTextureUAV(self, LightingBuffer, "output", RHIPipelineStageBits::ComputeShader,
                              RHITextureViewDesc{.format = RHIResourceFormat::R32G32B32A32SignedFloat,
                                                 .range = RHITextureSubresourceRange::Create()});

        },
        [=](PassHandle self, Renderer* r, RHICommandList* cmd)
        {
            r->CmdSetPipeline(self, cmd);
            r->CmdDispatch(self, cmd, {w, h, 1});
        });

    createPSFullscreenPassRTV(
        renderer, "Postprocess", PostprocessBuffer,
        RHITextureViewDesc{.format = postprocessFormat,
                           .range = RHITextureSubresourceRange::Create()},
        {w, h},
        [=](PassHandle self, Renderer* r)
        {
            r->BindShader(self, RHIShaderStageBits::Fragment, "fragMain",
                          Paths::Resolve("data/shaders/EPSPostprocess.spv"), AsBytes(AsSpan(cfg.viewFlags)));
            r->BindTextureSRV(self, LightingBuffer, "lighting", RHIPipelineStageBits::FragmentShader,
                              RHITextureViewDesc{.format = RHIResourceFormat::R32G32B32A32SignedFloat,
                                                 .range = RHITextureSubresourceRange::Create(
                                                     RHITextureAspectFlagBits::Color, 0, 1)});
            r->BindTextureSRV(self, OverdrawBuffer, "overdraw", RHIPipelineStageBits::FragmentShader,
                              RHITextureViewDesc{.format = RHIResourceFormat::R32Uint,
                                                 .range = RHITextureSubresourceRange::Create()});
            r->BindBufferUniform(self, GlobalUBO, RHIPipelineStageBits::FragmentShader, "globalParams");
            r->BindBufferStorageRead(self, ReduceBuffer, RHIPipelineStageBits::FragmentShader, "globalMax");
            r->BindTextureSRV(self, ViewLut, "viewLut", RHIPipelineStageBits::FragmentShader,
                              RHITextureViewDesc{.format = viewLutFormat,
                                                 .dimension = RHITextureDimension::E3D,
                                                 .range = RHITextureSubresourceRange::Create()});
            r->BindTextureSampler(self, LUTSampler, "lutSampler");
        },
        [](PassHandle, Renderer*, RHICommandList*) {});

    createPSFullscreenPass(
        renderer, "Blit Image",
        [=](PassHandle self, Renderer* r)
        {
            r->BindShader(self, RHIShaderStageBits::Fragment, "fragMain", Paths::Resolve("data/shaders/EPSBlit.spv"));
            r->BindTextureSRV(self, PostprocessBuffer, "displayImage", RHIPipelineStageBits::FragmentShader,
                              RHITextureViewDesc{.format = postprocessFormat,
                                                 .range = RHITextureSubresourceRange::Create(
                                                     RHITextureAspectFlagBits::Color, 0, 1)});
            r->BindTextureSRV(self, PickIDBuffer, "pickIDBuffer", RHIPipelineStageBits::FragmentShader,
                              RHITextureViewDesc{.format = RHIResourceFormat::R32Uint,
                                                 .range = RHITextureSubresourceRange::Create()});
            r->BindBufferUniform(self, GlobalUBO, RHIPipelineStageBits::FragmentShader, "globalParams");
            r->BindBufferUnordered(self, PickResultBuffer, RHIPipelineStageBits::FragmentShader, "pickResult");
            r->BindPushConstant(self, RHIShaderStageBits::Fragment, 0, sizeof(int2));
        },
        [=](PassHandle self, Renderer* r, RHICommandList* cmd)
        {
            // Push pick pixel coordinate every frame; (-1,-1) = no pending pick
            r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Fragment, 0, scene.picking->pendingPixel);
        });
    outHandles.hdrRT[0] = LightingBuffer;
    outHandles.numHdrRT = 1u;
    outHandles.sdrRT = PostprocessBuffer;
    outHandles.pickBuffer = PickResultBuffer;
}
