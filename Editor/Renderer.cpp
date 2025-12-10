#include "Renderer.hpp"
#include <RenderUtils/CSClearBuffer.hpp>
#include <RenderUtils/CSMipGeneration.hpp>
#include <RenderUtils/PSFullscreen.hpp>
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
void RendererSetupImGuiOnly(FContext* context)
{
    if (context->renderer)
        Destruct(context->allocator, context->renderer);
    auto* renderer = context->renderer = Construct<Renderer>(
        context->allocator, RendererDesc{.asyncCompute = false, .pipelineCache = context->psoCache.Get()},
        context->device, context->swapchain, context->allocator);
    renderer->BeginSetup();
    ImGui_ImplFoundation_CreatePass(renderer, "ImGui", true, FSetupDefault{});
    renderer->EndSetup();
}
// TODO: Make this part hot-reload?

void RendererSetup(FContext* context, UBO* pShaderGlobals, RendererConfig cfg)
{
    if (context->renderer)
        Destruct(context->allocator, context->renderer);
    auto* renderer = context->renderer = Construct<Renderer>(
        context->allocator, RendererDesc{.asyncCompute = true, .pipelineCache = context->psoCache.Get()},
        context->device, context->swapchain, context->allocator);
    auto* scene = context->gpuScene;
    renderer->BeginSetup();
    /* UBO for everyone */
    auto GlobalUBO = renderer->CreateResource(
        "Global UBO",
        RHIBufferDesc{.usage = RHIBufferUsageBits::TransferDestination | RHIBufferUsageBits::UniformBuffer,
                      .size = sizeof(UBO)});
    /* Instance and Primitive buffers */
    auto InstanceBuffer = renderer->CreateResource("Instance Buffer", scene->GetInstanceBuffer());
    auto PrimitiveBuffer = renderer->CreateResource("Primitive Buffer", scene->GetPrimitiveBuffer());
    auto MaterialBuffer = renderer->CreateResource("Material Buffer", scene->GetMaterialBuffer());
    /* Indirect Task Buffers */
    using enum RHIBufferUsageBits;
    auto IndirectTasks =
        renderer->CreateResource("Meshlet Indirect Tasks Buffer", // Instance IDs
                                 RHIBufferDesc{.usage = IndirectBuffer | StorageBuffer,
                                               .size = sizeof(MeshletTaskWork) * kMaxMeshletTaskWorkCount});
    auto IndirectTaskCounter = renderer->CreateResource(
        "Meshlet Indirect Tasks Counter (Single)",
        RHIBufferDesc{.usage = IndirectBuffer | StorageBuffer | TransferDestination, .size = sizeof(int)});
    // This only contain *one* dispatch command, which can spawn quite enough meshlet draws already!
    // See respective shaders for more details.
    auto IndirectTaskDispatch = renderer->CreateResource(
        "Meshlet Task Indirect Dispatch Command (Single)",
        RHIBufferDesc{.usage = IndirectBuffer | StorageBuffer, .size = sizeof(MeshletTaskDispatch)});
    // We pack meshlet visibility in uint32 bitmaps
    const size_t visBufferSize = AlignUp(kMaxMeshletCount, 32) / 32 * sizeof(uint32_t);
    auto OcclusionVisibility =
        renderer->CreateResource("Occlusion Visibility Buffer",
                                 RHIBufferDesc{.usage = StorageBuffer | TransferDestination, .size = visBufferSize});
    auto HistogramBins = renderer->CreateResource(
        "Histogram", RHIBufferDesc{.usage = StorageBuffer | TransferDestination, .size = sizeof(uint32_t) * 64});
    // NOTE: Lambda captures
    // NONE of the handle values outlive the renderer. Therefore, ALWAYS capture by value.
    renderer->CreatePass(
        "UBO Update & Init", RHIDeviceQueueType::Graphics, 0u,
        [=](PassHandle self, Renderer* r)
        {
            r->BindBufferCopyDst(self, GlobalUBO);
            r->BindBufferCopyDst(self, IndirectTaskCounter);
            r->BindBufferCopyDst(self, OcclusionVisibility);
            r->BindBufferCopyDst(self, HistogramBins);
        },
        [=](PassHandle, Renderer* r, RHICommandList* cmd)
        {
            auto* ubo = r->DerefResource(GlobalUBO).Get<RHIBuffer*>();
            auto* counter = r->DerefResource(IndirectTaskCounter).Get<RHIBuffer*>();
            auto* occlusion = r->DerefResource(OcclusionVisibility).Get<RHIBuffer*>();
            auto* histogram = r->DerefResource(HistogramBins).Get<RHIBuffer*>();
            // Fill, Update are considered Transfer operations
            // and would require proper barriers - which are automatically handled
            // by the Renderer *inter* passes.
            // Note that usage before a Dispatch, etc, may be valid but is still a ROW hazard.
            // TODO: Document these.
            cmd->UpdateBuffer(ubo, 0, AsBytes(AsSpan(*pShaderGlobals)));
            /// ^^^ Footgun as noted in MeshShaderHierarchicalLOD.cpp ^^^
            cmd->FillBuffer(counter, 0u);
            cmd->FillBuffer(occlusion, 0u);
            cmd->FillBuffer(histogram, 0u);
        });
    renderer->CreatePass(
        "Meshlet Task Generation", RHIDeviceQueueType::Graphics, 0u,
        [=](PassHandle self, Renderer* r)
        {
            r->BindShader(self, RHIShaderStageBits::Compute, "main", "data/shaders/ECSInstanceTaskCull.spv");
            r->BindBufferUniform(self, GlobalUBO, RHIPipelineStageBits::ComputeShader, "globalParams");
            r->BindBufferUnordered(self, IndirectTasks, RHIPipelineStageBits::ComputeShader, "tasks");
            r->BindBufferUnordered(self, IndirectTaskCounter, RHIPipelineStageBits::ComputeShader, "counter");
            r->BindBufferStorageRead(self, InstanceBuffer, RHIPipelineStageBits::ComputeShader, "instances");
            r->BindBufferStorageRead(self, PrimitiveBuffer, RHIPipelineStageBits::ComputeShader, "primitive");
        },
        [=](PassHandle self, Renderer* r, RHICommandList* cmd)
        {
            r->CmdSetPipeline(self, cmd);
            r->CmdDispatch(self, cmd, {pShaderGlobals->numInstances, 1, 1});
        });
    renderer->CreatePass(
        "Indirect Task Command Generation", RHIDeviceQueueType::Graphics, 0u,
        [=](PassHandle self, Renderer* r)
        {
            // Simply fills the dispatch buffer with the number of tasks to dispatch
            // A roundtrip back to the CPU would be expensive, so we do it all on the GPU side.
            r->BindShader(self, RHIShaderStageBits::Compute, "main", "data/shaders/ECSIndirectTaskGen.spv");
            r->BindBufferStorageRead(self, IndirectTaskCounter, RHIPipelineStageBits::ComputeShader, "counter");
            r->BindBufferUnordered(self, IndirectTaskDispatch, RHIPipelineStageBits::ComputeShader, "dispatch");
        },
        [=](PassHandle self, Renderer* r, RHICommandList* cmd)
        {
            r->CmdSetPipeline(self, cmd);
            r->CmdDispatch(self, cmd, {1, 1, 1});
        });
    /* Meshlet Drawing */
    auto [w, h] = renderer->GetSwapchainExtent();
    auto ZBuffer = renderer->CreateResource(
        "ZBuffer",
        RHITextureDesc{.usage = RHITextureUsageBits::DepthStencil | RHITextureUsageBits::SampledImage,
                       .extent = {w, h, 1},
                       .format = RHIResourceFormat::D32SignedFloat});
    uint32_t HIZWidth = 1u << log2(w / 2), HIZHeight = 1u << log2(h / 2);
    if (HIZWidth * 2 < w)
        HIZWidth *= 2;
    if (HIZHeight * 2 < w)
        HIZHeight *= 2;
    const uint32_t HIZMips = log2(std::max(HIZWidth, HIZHeight)) + 1u;
    const uint32_t FullMips = log2(std::max(w, h)) + 1u;
    pShaderGlobals->hizWidth = HIZWidth, pShaderGlobals->hizHeight = HIZHeight, pShaderGlobals->hizLevels = HIZMips;
    RHIDeviceSampler::SamplerDesc HIZSamplerDesc{
        .addressMode = {.u = RHIDeviceSampler::SamplerDesc::AddressMode::ClampToEdge,
                        .v = RHIDeviceSampler::SamplerDesc::AddressMode::ClampToEdge,
                        .w = RHIDeviceSampler::SamplerDesc::AddressMode::ClampToEdge},
        .mipmap = {.mipmapMode = RHIDeviceSampler::SamplerDesc::Mipmap::Nearest},
        .reduction = RHIDeviceSampler::SamplerDesc::Reduction::Min};
    auto HIZSampler = renderer->CreateSampler(HIZSamplerDesc);
    auto LinSampler = renderer->CreateSampler({});
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
                                                              .format = RHIResourceFormat::R8G8B8A8Unorm});
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
                r->BindShader(self, RHIShaderStageBits::Compute, "main", "data/shaders/ECSOverdrawClear.spv");
                r->BindPushConstant(self, RHIShaderStageBits::Compute, 0, sizeof(CSClearBufferData));
                r->BindBufferCopyDst(self, ReduceBuffer);
            },
            [=](PassHandle self, Renderer* r, RHICommandList* cmd)
            {
                auto* reduceBuffer = r->DerefResource(ReduceBuffer).Get<RHIBuffer*>();
                cmd->FillBuffer(reduceBuffer, 0u);
                RHIExtent2D wh = r->GetSwapchainExtent();
                CSClearBufferData cdata{float4{}, wh.x, wh.y};
                r->CmdSetPipeline(self, cmd);
                r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Compute, 0, cdata);
                r->CmdDispatch(self, cmd, {cdata.w, cdata.h, 1});
            });
    }
    /* Main Pass */
    {
        auto AddMainPass = [=](bool early)
        {
            renderer->CreatePass(
                early ? "Main [Stage 1]" : "Main [Stage 2]", RHIDeviceQueueType::Graphics, 0u,
                [=](PassHandle self, Renderer* r)
                {
                    int TSFlags = cfg.cullFlags;
                    if (early)
                        TSFlags |= kCullStageFirst;
                    else
                        TSFlags |= kCullStageLate;
                    r->BindShader(self, RHIShaderStageBits::Task, "main", "data/shaders/ETSMeshletCull.spv",
                                  AsBytes(AsSpan(TSFlags)));
                    r->BindShader(self, RHIShaderStageBits::Mesh, "main", "data/shaders/EMSBasic.spv");
                    r->BindShader(self, RHIShaderStageBits::Fragment, "main", "data/shaders/EPSGBuffer.spv",
                                  AsBytes(AsSpan(cfg.viewFlags)));
                    r->BindBufferUniform(self, GlobalUBO, RHIPipelineStageBits::ComputeShader, "globalParams");
                    r->BindBufferShaderRead(self, IndirectTaskDispatch,
                                            RHIPipelineStageBits::AllGraphics | RHIPipelineStageBits::DrawIndirect);
                    r->BindBufferStorageRead(self, IndirectTasks, RHIPipelineStageBits::ComputeShader, "tasks");
                    r->BindBufferStorageRead(self, IndirectTaskCounter, RHIPipelineStageBits::ComputeShader, "counter");
                    r->BindBufferStorageRead(self, InstanceBuffer, RHIPipelineStageBits::ComputeShader, "instances");
                    r->BindBufferStorageRead(self, PrimitiveBuffer, RHIPipelineStageBits::AllGraphics, "primitive");
                    r->BindBufferStorageRead(self, MaterialBuffer, RHIPipelineStageBits::AllGraphics, "materials");
                    r->BindBufferUnordered(self, OcclusionVisibility,
                                           RHIPipelineStageBits::AllGraphics | RHIPipelineStageBits::ComputeShader,
                                           "occlusion");
                    r->BindTextureRTV(self, GBufferRT0,
                                      {.format = RHIResourceFormat::R8G8B8A8Unorm,
                                       .range = RHITextureSubresourceRange::Create(RHITextureAspectFlagBits::Color)});
                    r->BindTextureRTV(self, GBufferRT1,
                                      {.format = RHIResourceFormat::R8G8B8A8Unorm,
                                       .range = RHITextureSubresourceRange::Create(RHITextureAspectFlagBits::Color)});
                    r->BindTextureRTV(self, GBufferRT2,
                                      {.format = RHIResourceFormat::R8G8B8A8Unorm,
                                       .range = RHITextureSubresourceRange::Create(RHITextureAspectFlagBits::Color)});
                    r->BindTextureUAV(self, OverdrawBuffer, "overdraw", RHIPipelineStageBits::FragmentShader,
                                      {.format = RHIResourceFormat::R32Uint,
                                       .range = RHITextureSubresourceRange::Create(RHITextureAspectFlagBits::Color)});
                    r->BindTextureDSV(self, ZBuffer,
                                      {.format = RHIResourceFormat::D32SignedFloat,
                                       .range = RHITextureSubresourceRange::Create(RHITextureAspectFlagBits::Depth)});
                    r->BindTextureSampler(self, HIZSampler, "hizSampler");
                    r->BindTextureSampler(self, LinSampler, "textureSampler");
                    r->BindTextureSRV(self, HIZ, "hiz",
                                      RHIPipelineStageBits::AllGraphics | RHIPipelineStageBits::ComputeShader,
                                      RHITextureViewDesc{.format = RHIResourceFormat::R32SignedFloat,
                                                         .range = RHITextureSubresourceRange::Create(
                                                             RHITextureAspectFlagBits::Color, 0, HIZMips)});
                    r->BindDescriptorSet(self, "textures",
                                         context->gpuScene->GetTexturePool()->GetDescriptorSetLayout());
                },
                [=](PassHandle self, Renderer* r, RHICommandList* cmd)
                {
                    RHIExtent2D wh{w, h};
                    auto* dispatchBuffer = r->DerefResource(IndirectTaskDispatch).Get<RHIBuffer*>();
                    if (early)
                        r->CmdBeginGraphics(
                            self, cmd, wh,
                            {{RHIClearColor{0, 0, 0, 0}, RHIClearColor{0, 0, 0, 0}, RHIClearColor{0, 0, 0, 0}}},
                            RHIClearDepthStencil{0.0f, 0});
                    else // Don't clear in stage 2 - we're appending false-positives back to it.
                        r->CmdBeginGraphics(self, cmd, wh, {{{}, {}, {}}}, {});
                    r->CmdSetPipeline(self, cmd);
                    r->CmdBindDescriptorSet(self, cmd, "textures",
                                            context->gpuScene->GetTexturePool()->GetDescriptorSet());
                    cmd->SetViewport(0, 0, w, h, 0, 1, true).SetScissor(0, 0, w, h);
                    cmd->DrawMeshTasksIndirect(dispatchBuffer, 0, 1, sizeof(MeshletTaskDispatch));
                    cmd->EndGraphics();
                });
            if (early && cfg.cullFlags & kCullOcclusion)
                createCSMipGenerationPasses(renderer, "Early HiZ", RHIDeviceQueueType::Graphics, ZBuffer, HIZ,
                                            RHIExtent2D{w, h}, RHITextureAspectFlagBits::Depth,
                                            RHIResourceFormat::D32SignedFloat, RHITextureAspectFlagBits::Color,
                                            RHIResourceFormat::R32SignedFloat, HIZMips, 0, HIZSamplerDesc);
        };
        AddMainPass(true);
        if (cfg.cullFlags & kCullOcclusion)
            AddMainPass(false);
    }
    if (cfg.viewFlags & kViewOverdraw)
    {
        renderer->CreatePass(
            "Overdraw CS Reduce", RHIDeviceQueueType::Graphics, 0u,
            [=](PassHandle self, Renderer* r)
            {
                r->BindShader(self, RHIShaderStageBits::Compute, "main", "data/shaders/ECSOverdrawReduce.spv");
                r->BindTextureSRV(self, OverdrawBuffer, "texture", RHIPipelineStageBits::ComputeShader,
                                  RHITextureViewDesc{.format = RHIResourceFormat::R32Uint,
                                                     .range = RHITextureSubresourceRange::Create()});
                r->BindBufferUnordered(self, ReduceBuffer, RHIPipelineStageBits::ComputeShader, "globalMax");
                r->BindPushConstant(self, RHIShaderStageBits::Compute, 0, sizeof(RHIExtent2D));
            },
            [=](PassHandle self, Renderer* r, RHICommandList* cmd)
            {
                RHIExtent2D wh = r->GetSwapchainExtent();
                r->CmdSetPipeline(self, cmd);
                r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Compute, 0, wh);
                r->CmdDispatch(self, cmd, {wh.x, wh.y, 1});
            });
    }
    auto LightingBuffer = renderer->CreateResource(
        "Lighting",
        RHITextureDesc{.usage = RHITextureUsageBits::StorageImage | RHITextureUsageBits::SampledImage,
                       .extent = {w, h, 1},
                       .format = RHIResourceFormat::B10G11R11Ufloat,
                       .mipLevels = FullMips});
    auto TLAS = renderer->CreateResource("Scene TLAS", scene->GetTLAS());
    renderer->CreatePass(
        "Lighting", RHIDeviceQueueType::Graphics, 0u,
        [=](PassHandle self, Renderer* r)
        {
            r->BindShader(self, RHIShaderStageBits::Compute, "main", "data/shaders/ECSLighting.spv",
                AsBytes(AsSpan(cfg.viewFlags)));
            r->BindBufferUniform(self, GlobalUBO, RHIPipelineStageBits::ComputeShader, "globalParams");
            r->BindTextureSRV(self, GBufferRT0, "RT0", RHIPipelineStageBits::ComputeShader,
                              RHITextureViewDesc{.format = RHIResourceFormat::R8G8B8A8Unorm,
                                                 .range = RHITextureSubresourceRange::Create()});
            r->BindTextureSRV(self, GBufferRT1, "RT1", RHIPipelineStageBits::ComputeShader,
                              RHITextureViewDesc{.format = RHIResourceFormat::R8G8B8A8Unorm,
                                                 .range = RHITextureSubresourceRange::Create()});
            r->BindTextureSRV(self, GBufferRT2, "RT2", RHIPipelineStageBits::ComputeShader,
                              RHITextureViewDesc{.format = RHIResourceFormat::R8G8B8A8Unorm,
                                                 .range = RHITextureSubresourceRange::Create()});
            r->BindTextureSRV(
                self, ZBuffer, "depth", RHIPipelineStageBits::ComputeShader,
                RHITextureViewDesc{.format = RHIResourceFormat::D32SignedFloat,
                                   .range = RHITextureSubresourceRange::Create(RHITextureAspectFlagBits::Depth)});
            r->BindBufferStorageRead(self, MaterialBuffer, RHIPipelineStageBits::ComputeShader, "materials");
            r->BindAcceleartionStructureSRV(self, TLAS, RHIPipelineStageBits::ComputeShader, "AS");
            r->BindTextureUAV(self, LightingBuffer, "output", RHIPipelineStageBits::ComputeShader,
                              RHITextureViewDesc{.format = RHIResourceFormat::B10G11R11Ufloat,
                                                 .range = RHITextureSubresourceRange::Create()});
        },
        [=](PassHandle self, Renderer* r, RHICommandList* cmd)
        {
            RHIExtent2D wh = r->GetSwapchainExtent();
            r->CmdSetPipeline(self, cmd);
            r->CmdDispatch(self, cmd, {wh.x, wh.y, 1});
        });
    renderer->CreatePass(
        "Histogram Binning", RHIDeviceQueueType::Graphics, 0u,
        [=](PassHandle self, Renderer* r)
        {
            r->BindShader(self, RHIShaderStageBits::Compute, "main", "data/shaders/ECSHistogramBinning.spv");
            r->BindBufferUniform(self, GlobalUBO, RHIPipelineStageBits::ComputeShader, "globalParams");
            r->BindTextureSRV(self, LightingBuffer, "lighting", RHIPipelineStageBits::ComputeShader,
                              RHITextureViewDesc{.format = RHIResourceFormat::B10G11R11Ufloat,
                                                 .range = RHITextureSubresourceRange::Create(
                                                     RHITextureAspectFlagBits::Color, 0, FullMips)});
            r->BindBufferUnordered(self, HistogramBins, RHIPipelineStageBits::ComputeShader, "bins");
        },
        [=](PassHandle self, Renderer* r, RHICommandList* cmd)
        {
            RHIExtent2D wh = r->GetSwapchainExtent();
            r->CmdSetPipeline(self, cmd);
            r->CmdDispatch(self, cmd, {wh.x, wh.y, 1});
        });
    auto LightingAverageLuma = renderer->CreateResource("Lighting Average Luminance",
                                                        RHIBufferDesc{.usage = StorageBuffer, .size = sizeof(float)});
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
            r->BindShader(self, RHIShaderStageBits::Fragment, "fragMain", "data/shaders/EPSBlit.spv",
                          AsBytes(AsSpan(cfg.viewFlags)));
            r->BindTextureSRV(self, LightingBuffer, "lighting", RHIPipelineStageBits::FragmentShader,
                              RHITextureViewDesc{.format = RHIResourceFormat::B10G11R11Ufloat,
                                                 .range = RHITextureSubresourceRange::Create(
                                                     RHITextureAspectFlagBits::Color, 0, FullMips)});
            r->BindTextureSRV(self, OverdrawBuffer, "overdraw", RHIPipelineStageBits::FragmentShader,
                              RHITextureViewDesc{.format = RHIResourceFormat::R32Uint,
                                                 .range = RHITextureSubresourceRange::Create()});
            r->BindBufferStorageRead(self, LightingAverageLuma, RHIPipelineStageBits::ComputeShader, "sceneLuma");
            r->BindBufferStorageRead(self, ReduceBuffer, RHIPipelineStageBits::FragmentShader, "globalMax");
        });
    ImGui_ImplFoundation_CreatePass(renderer, "ImGui", false, FSetupDefault{});
    renderer->EndSetup();
}
