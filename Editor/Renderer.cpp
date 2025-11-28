#include "Renderer.hpp"
#include <RenderUtils/CSClearBuffer.hpp>
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
constexpr size_t kMaxMeshletTaskWorkCount = 1e5;
void RendererSetupImGuiOnly(FContext* context)
{
    if (context->renderer)
        Destruct(context->allocator, context->renderer);
    auto* renderer = context->renderer =
        Construct<Renderer>(context->allocator, RendererDesc{.pipelineCache = context->psoCache.Get()}, context->device,
                            context->swapchain, context->allocator);
    renderer->BeginSetup();
    ImGui_ImplFoundation_CreatePass(renderer, "ImGui", true, FSetupDefault{});
    renderer->EndSetup();
}
// TODO: Make this part hot-reload?
void RendererSetup(FContext* context, UBO const* pShaderGlobals)
{
    if (context->renderer)
        Destruct(context->allocator, context->renderer);
    auto* renderer = context->renderer =
        Construct<Renderer>(context->allocator, RendererDesc{.pipelineCache = context->psoCache.Get()}, context->device,
                            context->swapchain, context->allocator);
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
    // NOTE: Lambda captures
    // NONE of the handle values outlive the renderer. Therefore, ALWAYS capture by value.
    renderer->CreatePass(
        "UBO Update", RHIDeviceQueueType::Compute, 0u,
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
            cmd->UpdateBuffer(ubo, 0, AsBytes(AsSpan(*pShaderGlobals)));
            /// ^^^ Footgun as noted in MeshShaderHierarchicalLOD.cpp ^^^
            cmd->FillBuffer(counter, 0u);
        });
    renderer->CreatePass(
        "Meshlet Task Generation", RHIDeviceQueueType::Compute, 0u,
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
        "Indirect Task Command Generation", RHIDeviceQueueType::Compute, 0u,
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
    auto ZBuffer = renderer->CreateResource("ZBuffer",
                                            RHITextureDesc{.usage = RHITextureUsageBits::DepthStencil,
                                                           .extent = {w, h, 1},
                                                           .format = RHIResourceFormat::D32SignedFloat});
    auto OverdrawBuffer = renderer->CreateResource("Overdraw Buffer",
                                                   RHITextureDesc{.usage = RHITextureUsageBits::RenderTarget |
                                                                      RHITextureUsageBits::StorageImage |
                                                                      RHITextureUsageBits::SampledImage,
                                                                  .extent = {w, h, 1},
                                                                  .format = RHIResourceFormat::R32Uint});
    auto GBuffer = renderer->CreateResource("GBuffer",
                                            RHITextureDesc{.usage = RHITextureUsageBits::RenderTarget |
                                                               RHITextureUsageBits::StorageImage |
                                                               RHITextureUsageBits::SampledImage,
                                                           .extent = {w, h, 1},
                                                           .format = RHIResourceFormat::R8G8B8A8Unorm});
    auto ReduceBuffer = renderer->CreateResource(
        "Reduced Values", RHIBufferDesc{.usage = StorageBuffer | TransferDestination, .size = sizeof(uint32_t) * 256});
    renderer->CreatePass(
        "Clear Overdraw+Reduce Buffer", RHIDeviceQueueType::Graphics, 0u,
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
    renderer->CreatePass(
        "Main Pass", RHIDeviceQueueType::Graphics, 0u,
        [=](PassHandle self, Renderer* r)
        {
            r->BindShader(self, RHIShaderStageBits::Task, "main", "data/shaders/ETSMeshletCull.spv");
            r->BindShader(self, RHIShaderStageBits::Mesh, "main", "data/shaders/EMSBasic.spv");
            r->BindShader(self, RHIShaderStageBits::Fragment, "main", "data/shaders/EPSBasic.spv");
            r->BindBufferUniform(self, GlobalUBO, RHIPipelineStageBits::ComputeShader, "globalParams");
            r->BindBufferShaderRead(self, IndirectTaskDispatch,
                                    RHIPipelineStageBits::AllGraphics | RHIPipelineStageBits::DrawIndirect);
            r->BindBufferStorageRead(self, IndirectTasks, RHIPipelineStageBits::ComputeShader, "tasks");
            r->BindBufferStorageRead(self, IndirectTaskCounter, RHIPipelineStageBits::ComputeShader, "counter");
            r->BindBufferStorageRead(self, InstanceBuffer, RHIPipelineStageBits::ComputeShader, "instances");
            r->BindBufferStorageRead(self, PrimitiveBuffer, RHIPipelineStageBits::AllGraphics, "primitive");
            r->BindTextureRTV(self, GBuffer,
                              {.format = RHIResourceFormat::R8G8B8A8Unorm,
                               .range = RHITextureSubresourceRange::Create(RHITextureAspectFlagBits::Color)});
            r->BindTextureUAV(self, OverdrawBuffer, "overdraw", RHIPipelineStageBits::FragmentShader,
                              {.format = RHIResourceFormat::R32Uint,
                               .range = RHITextureSubresourceRange::Create(RHITextureAspectFlagBits::Color)});
            r->BindTextureDSV(self, ZBuffer,
                              {.format = RHIResourceFormat::D32SignedFloat,
                               .range = RHITextureSubresourceRange::Create(RHITextureAspectFlagBits::Depth)});
        },
        [=](PassHandle self, Renderer* r, RHICommandList* cmd)
        {
            RHIExtent2D wh{w, h};
            auto* dispatchBuffer = r->DerefResource(IndirectTaskDispatch).Get<RHIBuffer*>();
            r->CmdBeginGraphics(self, cmd, wh, {{RHIClearColor{0, 0, 0, 0}}}, RHIClearDepthStencil{0.0f, 0});
            r->CmdSetPipeline(self, cmd);
            cmd->SetViewport(0, 0, w, h, 0, 1, true).SetScissor(0, 0, w, h);
            cmd->DrawMeshTasksIndirect(dispatchBuffer, 0, 1, sizeof(MeshletTaskDispatch));
            cmd->EndGraphics();
        });

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
    auto nearSampler =
        renderer->CreateSampler({.filter = {.minFilter = RHIDeviceSampler::SamplerDesc::Filter::NearestNeighbor,
                                            .magFilter = RHIDeviceSampler::SamplerDesc::Filter::NearestNeighbor}});
    createPSFullscreenPass(renderer, "Blit Image",
                           [=](PassHandle self, Renderer* r)
                           {
                               uint32_t mode = 0;
                               r->BindShader(self, RHIShaderStageBits::Fragment, "fragMain", "data/shaders/EPSBlit.spv",
                                             AsBytes(AsSpan(mode)));
                               r->BindTextureSampler(self, nearSampler, "sampler");
                               r->BindTextureSRV(self, GBuffer, "gbuffer", RHIPipelineStageBits::FragmentShader,
                                                 RHITextureViewDesc{.format = RHIResourceFormat::R8G8B8A8Unorm,
                                                                    .range = RHITextureSubresourceRange::Create()});
                               r->BindTextureSRV(self, OverdrawBuffer, "overdraw", RHIPipelineStageBits::FragmentShader,
                                                 RHITextureViewDesc{.format = RHIResourceFormat::R32Uint,
                                                                    .range = RHITextureSubresourceRange::Create()});
                               r->BindBufferStorageRead(self, ReduceBuffer, RHIPipelineStageBits::FragmentShader,
                                                        "globalMax");
                           });
    ImGui_ImplFoundation_CreatePass(renderer, "ImGui", false, FSetupDefault{});
    renderer->EndSetup();
}
