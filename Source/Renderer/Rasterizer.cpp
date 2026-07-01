#include <RenderUtils/CSClearBuffer.hpp>
#include <RenderUtils/CSMipGeneration.hpp>
#include <RenderUtils/PSFullscreen.hpp>
#include <algorithm>
#include <Core/Paths.hpp>
#include "GPUScene.hpp"
#include "Renderer.hpp"
using namespace Foundation;
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
// Mirrors VkDrawIndexedIndirectCommand. One per dynamic (CPU-updateable) geometry instance,
// produced by ECSIndirectDraw and consumed by DrawIndexedIndirectCount.
struct DrawIndexedIndirectCommand
{
    uint32_t indexCount;
    uint32_t instanceCount;
    uint32_t firstIndex;
    int32_t vertexOffset;
    uint32_t firstInstance;
};
#pragma pack(pop)
constexpr size_t kMeshWorkGroupSize = 64;
constexpr size_t kMaxMeshletCount = 1e6;
constexpr size_t kMaxMeshletTaskWorkCount = kMaxMeshletCount / kMeshWorkGroupSize;
constexpr size_t kMaxDynamicDraws = 4096; // dynamic geometry instances drawn per frame (raster)
constexpr size_t kDisableRTBuildFlags = kViewOverdraw | kViewMeshlet | kViewBaseColor | kViewNormal | kViewPosition | kViewMatcap;
void BuildRasterRenderGraph(Renderer* renderer, RendererUBO* globals, GPUScene* gpu,
                            RendererConfig const& cfg, RendererOutputs& out)
{
    CHECK(renderer);
    CHECK(globals);
    CHECK(gpu);
    CHECK_MSG(renderer->GetDevice()->GetCapabilities().meshShaders, "Rasterizer requires Mesh Shader support");
    out = {};
    RHIExtent2D renderExtent = cfg.renderExtent;
    if (renderExtent.x == 0u || renderExtent.y == 0u)
        renderExtent = renderer->GetSwapchainExtent();
    // Framebuffer extents are renderer-owned: stamped from the resolved cfg.renderExtent
    // (matching the actual render-target sizes) so the host UBO and GPU UBO stay in sync.
    globals->fbWidth = static_cast<float>(renderExtent.x);
    globals->fbHeight = static_cast<float>(renderExtent.y);
    /* UBO for everyone */
    auto GlobalUBO = renderer->CreateResource(
        "Global UBO",
        RHIBufferDesc{.usage = RHIBufferUsageBits::TransferDestination | RHIBufferUsageBits::UniformBuffer,
                      .size = sizeof(RendererUBO)});
    /* Instance and Primitive buffers */
    bool hasTLAS = gpu->GetTLAS() != nullptr;
    ResourceHandle TLAS = kInvalidHandle;
    if (hasTLAS)
        TLAS = renderer->CreateResource("Scene TLAS", gpu->GetTLAS());
    auto PrimitiveBuffer = renderer->CreateResource("Primitive Buffer", gpu->GetPrimitiveBuffer());
    auto DynamicPrimitiveBuffer = renderer->CreateResource("Dynamic Primitive Buffer", gpu->GetDynamicPrimitiveBuffer());

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
            cmd->UpdateBuffer(ubo, 0, AsBytes(AsSpan(*globals)));
            cmd->FillBuffer(counter, 0u);
        });
    bool disableRT = cfg.viewFlags & kDisableRTBuildFlags;
    bool useRTShadows = (cfg.viewFlags & kEnableRasterRTShadows) && !disableRT && hasTLAS;
    uint32_t lightingViewFlags = useRTShadows ? cfg.viewFlags : (cfg.viewFlags & ~kEnableRasterRTShadows);
    uint32_t gbufferFlags = cfg.viewFlags | (cfg.forceTextureLOD0 ? kForceTextureLOD0 : 0u);
    // Raytracing. The raster path draws dynamic geometry through its own vertex/index MDI draw
    // (no BLAS needed for shading), so the dynamic BLAS refit is only required when RT shadows
    // trace the TLAS - which reads the dynamic BLASes.
    if (useRTShadows)
    {
        renderer->CreatePass(
            "TLAS/BLAS Update", RHIDeviceQueueType::Compute, 0u, [=](PassHandle self, Renderer* r)
            { r->BindAccelerationStructureWrite(self, TLAS); }, [=](PassHandle, Renderer* r, RHICommandList* cmd)
            {
                if (gpu->HasDynamicGeometry())
                    gpu->BuildBLAS(cmd);
                (void)gpu->BuildTLAS(cmd, true);
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
            r->BindShader(self, RHIShaderStageBits::Compute, "main", PathsResolve("Data/Shaders/ECSCullInstances.spv"));
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
            r->CmdDispatch(self, cmd, {globals->numInstances, 1, 1});
        });
    /* Meshlet Drawing */
    uint32_t w = std::max(renderExtent.x, 16u);
    uint32_t h = std::max(renderExtent.y, 16u);
    auto Depth = renderer->CreateResource(
        "Depth",
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
    const uint32_t HIZMips = std::min(glm::log2(std::max(HIZWidth, HIZHeight)) + 1u, 12u);
    const uint32_t HIZMaxDimension = 1u << (HIZMips - 1);
    while (HIZWidth > HIZMaxDimension)
        HIZWidth >>= 1;
    while (HIZHeight > HIZMaxDimension)
		HIZHeight >>= 1;
    globals->hizWidth = HIZWidth, globals->hizHeight = HIZHeight, globals->hizLevels = HIZMips;
    RHIDeviceSampler::SamplerDesc HIZSamplerDesc{
        .addressMode = {.u = RHIDeviceSampler::SamplerDesc::AddressMode::ClampToEdge,
                        .v = RHIDeviceSampler::SamplerDesc::AddressMode::ClampToEdge,
                        .w = RHIDeviceSampler::SamplerDesc::AddressMode::ClampToEdge},
        .mipmap = {.mipmapMode = RHIDeviceSampler::SamplerDesc::Mipmap::Nearest},
        .reduction = RHIDeviceSampler::SamplerDesc::Reduction::Min};
    auto HIZSampler = renderer->CreateSampler(HIZSamplerDesc);
    auto TexSampler = renderer->CreateSampler(MakeTextureSamplerDesc(cfg));
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
                                                              .format = RHIResourceFormat::B10G11R11Ufloat});
    // Instance ID map: R32_UINT, one uint per pixel storing the absolute instance index.
    // ~0u means "no object" (cleared each frame).
    auto InstanceIDBuffer = renderer->CreateResource("Instance ID Buffer",
                                                    RHITextureDesc{.usage = RHITextureUsageBits::RenderTarget |
                                                                       RHITextureUsageBits::SampledImage,
                                                                   .extent = {w, h, 1},
                                                                   .format = RHIResourceFormat::R32Uint});
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
                r->BindShader(self, RHIShaderStageBits::Compute, "main", PathsResolve("Data/Shaders/ECSOverdrawClear.spv"));
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
                    r->BindShader(self, RHIShaderStageBits::Compute, "main", PathsResolve("Data/Shaders/ECSCullMeshlets.spv"),
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
                    // r->BindShader(self, RHIShaderStageBits::Task, "main", PathsResolve("Data/Shaders/ETSMeshletCull.spv"),
                    // AsBytes(AsSpan(TSFlags)));
                    r->BindShader(self, RHIShaderStageBits::Mesh, "main", PathsResolve("Data/Shaders/EMSBasic.spv"));
                    r->BindShader(self, RHIShaderStageBits::Fragment, "main", PathsResolve("Data/Shaders/EPSGBuffer.spv"),
                                  AsBytes(AsSpan(gbufferFlags)));
                    r->BindBufferUniform(self, GlobalUBO, RHIPipelineStageBits::AllGraphics, "globalParams");
                    r->BindBufferStorageRead(self, PrimitiveBuffer, RHIPipelineStageBits::AllGraphics, "primitive");
                    r->BindBufferStorageRead(self, InstanceBuffer, RHIPipelineStageBits::AllGraphics, "instances");
                    r->BindBufferStorageRead(self, MaterialBuffer, RHIPipelineStageBits::AllGraphics, "materials");
                    r->BindBufferStorageRead(self, IndirectMeshletCounter, RHIPipelineStageBits::AllGraphics, "inMeshletCounter");
                    r->BindBufferStorageRead(self, IndirectMeshlets, RHIPipelineStageBits::AllGraphics, "inMeshletIndices");
                    r->BindTextureRTV(self, GBufferRT0,
                                      {.format = RHIResourceFormat::R8G8B8A8Unorm,
                                       .range = RHITextureSubresourceRange::Create(RHITextureAspectFlagBits::Color)});
                    r->BindTextureRTV(self, GBufferRT1,
                                      {.format = RHIResourceFormat::R8G8B8A8Unorm,
                                       .range = RHITextureSubresourceRange::Create(RHITextureAspectFlagBits::Color)});
                    r->BindTextureRTV(self, GBufferRT2,
                                      {.format = RHIResourceFormat::B10G11R11Ufloat,
                                       .range = RHITextureSubresourceRange::Create(RHITextureAspectFlagBits::Color)});
                    r->BindTextureRTV(self, InstanceIDBuffer,
                                      {.format = RHIResourceFormat::R32Uint,
                                       .range = RHITextureSubresourceRange::Create(RHITextureAspectFlagBits::Color)});
                    r->BindTextureUAV(self, OverdrawBuffer, "overdraw", RHIPipelineStageBits::FragmentShader,
                                      {.format = RHIResourceFormat::R32Uint,
                                       .range = RHITextureSubresourceRange::Create(RHITextureAspectFlagBits::Color)});
                    r->BindTextureDSV(self, Depth,
                                      {.format = RHIResourceFormat::D32SignedFloat,
                                       .range = RHITextureSubresourceRange::Create(RHITextureAspectFlagBits::Depth)});
                    r->BindBufferIndirectRead(self, IndirectMeshletDispatch);
                    r->BindTextureSampler(self, TexSampler, "textureSampler");
                    r->BindDescriptorSet(self, "textures",
                                         gpu->GetTexture2DPool()->GetDescriptorSetLayout());
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
                                            gpu->GetTexture2DPool()->GetDescriptorSet());
                    auto* dispatchBuffer = r->DerefResource(IndirectMeshletDispatch).Get<RHIBuffer*>();
                    cmd->DrawMeshTasksIndirect(dispatchBuffer, 0, 1, sizeof(MeshletTaskDispatch));
                    cmd->EndGraphics();
                });
            if (early && cfg.cullFlags & kCullOcclusion)
            {
                // TODO: Single pass currently present higher register pressure than expected (72 VGPRs?)
                //       Figure out where I messed up.                
                if (true)
                    createCSMipGenerationSinglePass(renderer, "Early HiZ Mip Gen", RHIDeviceQueueType::Graphics, Depth,
                                                    HIZ, RHIResourceFormat::D32SignedFloat,
                                                    RHIResourceFormat::R32SignedFloat, RHITextureAspectFlagBits::Depth,
                                                    RHITextureAspectFlagBits::Color, HIZSampler, HIZMips, 1,
                                                    HIZSamplerDesc.reduction);
                else
                    createCSMipGenerationPasses(renderer, "Early HiZ Mip Gen", RHIDeviceQueueType::Graphics, Depth, HIZ,
                                                RHIResourceFormat::D32SignedFloat, RHIResourceFormat::R32SignedFloat,
                                                RHITextureAspectFlagBits::Depth, RHITextureAspectFlagBits::Color,
                                                HIZSampler, HIZMips);
            }
        };
        AddCullPass(true), AddMainPass(true);
        if (cfg.cullFlags & kCullOcclusion)
            AddCullPass(false), AddMainPass(false);
    }
    /* Dynamic (CPU-updateable) geometry: drawn outside the meshlet pipeline (no DAG/meshlets)
       via a GPU-driven indexed multi-draw into the same gbuffer, depth-tested against it. */
    if (gpu->HasDynamicGeometry())
    {
        auto DynamicDrawCmds = renderer->CreateResource(
            "Dynamic Draw Commands",
            RHIBufferDesc{.usage = IndirectBuffer | StorageBuffer | TransferDestination,
                          .size = sizeof(DrawIndexedIndirectCommand) * kMaxDynamicDraws});
        auto DynamicDrawCount = renderer->CreateResource(
            "Dynamic Draw Count",
            RHIBufferDesc{.usage = IndirectBuffer | StorageBuffer | TransferDestination, .size = sizeof(uint32_t)});
        auto DynamicDrawInstanceIDs = renderer->CreateResource(
            "Dynamic Draw Instance IDs",
            RHIBufferDesc{.usage = StorageBuffer | TransferDestination, .size = sizeof(uint32_t) * kMaxDynamicDraws});
        renderer->CreatePass(
            "Dynamic Draw Gen", RHIDeviceQueueType::Graphics, 0u,
            [=](PassHandle self, Renderer* r)
            {
                r->BindBufferCopyDst(self, DynamicDrawCount);
                r->BindShader(self, RHIShaderStageBits::Compute, "main",
                              PathsResolve("Data/Shaders/ECSIndirectDraw.spv"));
                r->BindBufferUniform(self, GlobalUBO, RHIPipelineStageBits::ComputeShader, "globalParams");
                r->BindBufferStorageRead(self, InstanceBuffer, RHIPipelineStageBits::ComputeShader, "instances");
                r->BindBufferStorageRead(self, DynamicPrimitiveBuffer, RHIPipelineStageBits::ComputeShader,
                                         "dynamicPrimitives");
                r->BindBufferUnordered(self, DynamicDrawCmds, RHIPipelineStageBits::ComputeShader, "outDrawCmds");
                r->BindBufferUnordered(self, DynamicDrawInstanceIDs, RHIPipelineStageBits::ComputeShader,
                                       "outDrawInstanceIDs");
                r->BindBufferUnordered(self, DynamicDrawCount, RHIPipelineStageBits::ComputeShader, "outDrawCount");
            },
            [=](PassHandle self, Renderer* r, RHICommandList* cmd)
            {
                auto* count = r->DerefResource(DynamicDrawCount).Get<RHIBuffer*>();
                cmd->FillBuffer(count, 0u);
                r->CmdSetPipeline(self, cmd);
                r->CmdDispatch(self, cmd, {globals->numInstances, 1, 1});
            });
        renderer->CreatePass(
            "Dynamic GBuffer", RHIDeviceQueueType::Graphics, 0u,
            [=](PassHandle self, Renderer* r)
            {
                r->BindShader(self, RHIShaderStageBits::Vertex, "main",
                              PathsResolve("Data/Shaders/EVSIndirectDraw.spv"));
                // Reuses the meshlet path's gbuffer fragment (bindings 3..5 sit right after this
                // VS's 0..2, keeping set 0 contiguous).
                r->BindShader(self, RHIShaderStageBits::Fragment, "main", PathsResolve("Data/Shaders/EPSGBuffer.spv"),
                              AsBytes(AsSpan(gbufferFlags)));
                r->BindBufferUniform(self, GlobalUBO, RHIPipelineStageBits::AllGraphics, "globalParams");
                r->BindBufferStorageRead(self, InstanceBuffer, RHIPipelineStageBits::AllGraphics, "instances");
                r->BindBufferStorageRead(self, DynamicPrimitiveBuffer, RHIPipelineStageBits::AllGraphics,
                                         "dynamicPrimitives");
                r->BindBufferStorageRead(self, DynamicDrawInstanceIDs, RHIPipelineStageBits::AllGraphics,
                                         "drawInstanceIDs");
                r->BindBufferStorageRead(self, MaterialBuffer, RHIPipelineStageBits::AllGraphics, "materials");
                r->BindTextureRTV(self, GBufferRT0,
                                  {.format = RHIResourceFormat::R8G8B8A8Unorm,
                                   .range = RHITextureSubresourceRange::Create(RHITextureAspectFlagBits::Color)});
                r->BindTextureRTV(self, GBufferRT1,
                                  {.format = RHIResourceFormat::R8G8B8A8Unorm,
                                   .range = RHITextureSubresourceRange::Create(RHITextureAspectFlagBits::Color)});
                r->BindTextureRTV(self, GBufferRT2,
                                  {.format = RHIResourceFormat::B10G11R11Ufloat,
                                   .range = RHITextureSubresourceRange::Create(RHITextureAspectFlagBits::Color)});
                r->BindTextureRTV(self, InstanceIDBuffer,
                                  {.format = RHIResourceFormat::R32Uint,
                                   .range = RHITextureSubresourceRange::Create(RHITextureAspectFlagBits::Color)});
                r->BindTextureUAV(self, OverdrawBuffer, "overdraw", RHIPipelineStageBits::FragmentShader,
                                  {.format = RHIResourceFormat::R32Uint,
                                   .range = RHITextureSubresourceRange::Create(RHITextureAspectFlagBits::Color)});
                r->BindTextureDSV(self, Depth,
                                  {.format = RHIResourceFormat::D32SignedFloat,
                                   .range = RHITextureSubresourceRange::Create(RHITextureAspectFlagBits::Depth)});
                r->BindBufferIndirectRead(self, DynamicDrawCmds);
                r->BindBufferIndirectRead(self, DynamicDrawCount);
                r->BindTextureSampler(self, TexSampler, "textureSampler");
                r->BindDescriptorSet(self, "textures", gpu->GetTexture2DPool()->GetDescriptorSetLayout());
            },
            [=](PassHandle self, Renderer* r, RHICommandList* cmd)
            {
                RHIExtent2D wh{w, h};
                // LOAD the meshlet-produced gbuffer + depth; dynamic geo depth-tests against it.
                r->CmdBeginGraphics(self, cmd, wh,
                                    {{{RHIAttachmentLoadOp::Load},
                                      {RHIAttachmentLoadOp::Load},
                                      {RHIAttachmentLoadOp::Load},
                                      {RHIAttachmentLoadOp::Load}}},
                                    {RHIAttachmentLoadOp::Load});
                r->CmdSetPipeline(self, cmd);
                cmd->SetViewport(0, 0, w, h, 0, 1, true).SetScissor(0, 0, w, h);
                r->CmdBindDescriptorSet(self, cmd, "textures", gpu->GetTexture2DPool()->GetDescriptorSet());
                // The dynamic ring is bound as a UINT32 index buffer; the VS pulls vertices from
                // it by SV_VertexID and reads the instance via SV_InstanceID (firstInstance).
                cmd->BindIndexBuffer(gpu->GetDynamicPrimitiveBuffer(), 0, RHIResourceFormat::R32Uint);
                auto* cmds = r->DerefResource(DynamicDrawCmds).Get<RHIBuffer*>();
                auto* count = r->DerefResource(DynamicDrawCount).Get<RHIBuffer*>();
                cmd->DrawIndexedIndirectCount(cmds, 0, count, 0, kMaxDynamicDraws, sizeof(DrawIndexedIndirectCommand));
                cmd->EndGraphics();
            });
    }
    if (cfg.viewFlags & kViewOverdraw)
    {
        renderer->CreatePass(
            "Overdraw CS Reduce", RHIDeviceQueueType::Compute, 0u,
            [=](PassHandle self, Renderer* r)
            {
                r->BindShader(self, RHIShaderStageBits::Compute, "main", PathsResolve("Data/Shaders/ECSOverdrawReduce.spv"));
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
    ResourceHandle DebugOutput = kInvalidHandle;
    // Debug views
    if (cfg.viewFlags & kViewOverdraw)
    {
        DebugOutput = renderer->CreateResource(
            "Overdraw Debug Output",
            RHITextureDesc{.usage = RHITextureUsageBits::RenderTarget |
                                      RHITextureUsageBits::SampledImage |
                                      RHITextureUsageBits::TransferSource,
                           .extent = {w, h, 1},
                           .format = RHIResourceFormat::R8G8B8A8Unorm});
        createPSFullscreenPassRTV(
            renderer, "Overdraw Debug", DebugOutput,
            RHITextureViewDesc{.format = RHIResourceFormat::R8G8B8A8Unorm,
                               .range = RHITextureSubresourceRange::Create()},
            [=](PassHandle self, Renderer* r)
            {
                r->BindShader(self, RHIShaderStageBits::Fragment, "fragMain",
                              PathsResolve("Data/Shaders/EPSOverdrawDebug.spv"));
                r->BindTextureSRV(self, OverdrawBuffer, "overdraw", RHIPipelineStageBits::FragmentShader,
                                  RHITextureViewDesc{.format = RHIResourceFormat::R32Uint,
                                                     .range = RHITextureSubresourceRange::Create()});
                r->BindBufferStorageRead(self, ReduceBuffer, RHIPipelineStageBits::FragmentShader, "globalMax");
            });
    } else
    {
        auto DiffuseBuffer = renderer->CreateResource(
            "Diffuse",
            RHITextureDesc{.usage = RHITextureUsageBits::StorageImage |
                                      RHITextureUsageBits::SampledImage |
                                      RHITextureUsageBits::TransferSource,
                           .extent = {w, h, 1},
                           .format = RHIResourceFormat::R32G32B32A32SignedFloat});
        auto SpecularBuffer = renderer->CreateResource(
            "Specular",
            RHITextureDesc{.usage = RHITextureUsageBits::StorageImage |
                                      RHITextureUsageBits::SampledImage |
                                      RHITextureUsageBits::TransferSource,
                           .extent = {w, h, 1},
                           .format = RHIResourceFormat::R32G32B32A32SignedFloat});
        auto LUTSampler = renderer->CreateSampler({
            .addressMode = {
                .u = RHIDeviceSampler::SamplerDesc::AddressMode::ClampToEdge,
                .v = RHIDeviceSampler::SamplerDesc::AddressMode::ClampToEdge,
                .w = RHIDeviceSampler::SamplerDesc::AddressMode::ClampToEdge,
            }
        });
        renderer->CreatePass(
            "Lighting", RHIDeviceQueueType::Graphics, 0u,
            [=](PassHandle self, Renderer* r)
            {
                r->BindShader(self, RHIShaderStageBits::Compute, "main", PathsResolve("Data/Shaders/ECSLighting.spv"),
                              AsBytes(AsSpan(lightingViewFlags)));
                r->BindBufferUniform(self, GlobalUBO, RHIPipelineStageBits::ComputeShader, "globalParams");
                r->BindTextureSRV(self, GBufferRT0, "RT0", RHIPipelineStageBits::ComputeShader,
                                  RHITextureViewDesc{.format = RHIResourceFormat::R8G8B8A8Unorm,
                                                     .range = RHITextureSubresourceRange::Create()});
                r->BindTextureSRV(self, GBufferRT1, "RT1", RHIPipelineStageBits::ComputeShader,
                                  RHITextureViewDesc{.format = RHIResourceFormat::R8G8B8A8Unorm,
                                                     .range = RHITextureSubresourceRange::Create()});
                r->BindTextureSRV(self, GBufferRT2, "RT2", RHIPipelineStageBits::ComputeShader,
                                  RHITextureViewDesc{.format = RHIResourceFormat::B10G11R11Ufloat,
                                                     .range = RHITextureSubresourceRange::Create()});
                r->BindTextureSRV(
                    self, Depth, "depth", RHIPipelineStageBits::ComputeShader,
                    RHITextureViewDesc{.format = RHIResourceFormat::D32SignedFloat,
                                       .range = RHITextureSubresourceRange::Create(RHITextureAspectFlagBits::Depth)});
                r->BindAccelerationStructureSRV(self, TLAS, RHIPipelineStageBits::ComputeShader, "AS");
                r->BindBufferStorageRead(self, LightBuffer, RHIPipelineStageBits::ComputeShader, "lights");
                r->BindTextureSampler(self, LUTSampler, "lutSampler");
                r->BindDescriptorSet(self, "textures", gpu->GetTexture2DPool()->GetDescriptorSetLayout());
                r->BindDescriptorSet(self, "textures3D", gpu->GetTexture3DPool()->GetDescriptorSetLayout());
                r->BindTextureUAV(self, DiffuseBuffer, "diffuseOutput", RHIPipelineStageBits::ComputeShader,
                                  RHITextureViewDesc{.format = RHIResourceFormat::R32G32B32A32SignedFloat,
                                                     .range = RHITextureSubresourceRange::Create()});
                r->BindTextureUAV(self, SpecularBuffer, "specularOutput", RHIPipelineStageBits::ComputeShader,
                                  RHITextureViewDesc{.format = RHIResourceFormat::R32G32B32A32SignedFloat,
                                                     .range = RHITextureSubresourceRange::Create()});

            },
            [=](PassHandle self, Renderer* r, RHICommandList* cmd)
            {
                r->CmdSetPipeline(self, cmd);
                r->CmdBindDescriptorSet(self, cmd, "textures", gpu->GetTexture2DPool()->GetDescriptorSet());
                r->CmdBindDescriptorSet(self, cmd, "textures3D", gpu->GetTexture3DPool()->GetDescriptorSet());
                r->CmdDispatch(self, cmd, {w, h, 1});
            });
        out.diffuse = DiffuseBuffer;
        out.specular = SpecularBuffer;
    }
    out.extent = {w, h};
    out.depth = Depth;
    out.debugOutput = DebugOutput;
    out.instanceID = InstanceIDBuffer;
}
