#include <Bindings/ImGui.hpp>
#include <Rendering/PSFullscreen.hpp>
#include <Rendering/CSMipGeneration.hpp>
#include "App.hpp"
#include "Assets/Mesh.hpp"
using namespace ModelViewer;
const size_t kMaxMeshletTasks = 1e5;
struct MeshletTaskParams
{
    uint32_t instanceID;
    uint32_t vertexRawOffset;
    uint32_t meshletCount;
    uint32_t meshletRawOffset;
    uint32_t meshletVerticesRawOffset;
    uint32_t meshletTrianglesRawOffset;
};
#pragma pack(push, 1)
struct MeshletTaskDispatch // VkDrawMeshTasksIndirectCommandEXT
{
    uint32_t groupCountX;
    uint32_t groupCountY;
    uint32_t groupCountZ;
};
#pragma pack(pop)
void App::OnRendererSetup()
{
    ResourceHandle meshletTaskParams =
        createResource(mRenderer.get(), "Meshlet Tasks",
                       RHIBufferDesc{.usage = RHIBufferUsageBits::StorageBuffer,
                                     .size = sizeof(MeshletTaskParams) * kMaxMeshletTasks});
    ResourceHandle meshletTaskDispatch =
        createResource(mRenderer.get(), "Meshlet Task Submit Command Buffer [single]",
                       RHIBufferDesc{.usage = RHIBufferUsageBits::IndirectBuffer | RHIBufferUsageBits::StorageBuffer |
                                         RHIBufferUsageBits::TransferDestination,
                                     .size = sizeof(MeshletTaskDispatch)});
    ResourceHandle meshletIndirectTasksCtr =
        createResource(mRenderer.get(), "Meshlet Indirect Tasks Counter [single]",
                       RHIBufferDesc{.usage = RHIBufferUsageBits::IndirectBuffer | RHIBufferUsageBits::StorageBuffer |
                                         RHIBufferUsageBits::TransferDestination,
                                     .size = sizeof(int)});
    CHECK_MSG(mViewportSize.x <= kTextureMaxExtent.x && mViewportSize.y <= kTextureMaxExtent.y,
              "Viewport size ({}x{}) exceeds swapchain extent ({}x{})", mViewportSize.x, mViewportSize.y,kTextureMaxExtent.x, kTextureMaxExtent.y);
    ResourceHandle zbuffer = createResource(mRenderer.get(), "ZBuffer",
                                            RHITextureDesc{
                                                .usage = RHITextureUsageBits::DepthStencil | RHITextureUsageBits::SampledImage,
                                                .extent = kTextureMaxExtent,
                                                .format = RHIResourceFormat::D32SignedFloat,
                                            });
    ResourceHandle hiz = createResource(mRenderer.get(), "HiZ",
        RHITextureDesc{
            .usage = RHITextureUsageBits::SampledImage | RHITextureUsageBits::StorageImage,
            .extent = kTextureMaxExtent,
            .format = RHIResourceFormat::R32SignedFloat,
            .mipLevels = static_cast<uint32_t>(std::floor(std::log2(std::max(kTextureMaxExtent.x, kTextureMaxExtent.y)))) + 1,
        });
    ResourceHandle gbuffer =
        createResource(mRenderer.get(), "GBuffer",
                       RHITextureDesc{
                           .usage = RHITextureUsageBits::RenderTarget | RHITextureUsageBits::SampledImage,
                           .extent = kTextureMaxExtent,
                           .format = RHIResourceFormat::R8G8B8A8Unorm,
                       });
    ResourceHandle sceneInstance, sceneShared, sceneConst;
    mGPUScene->CreateUpdatePasses(mRenderer.get(), sceneInstance, sceneShared, sceneConst,
                                  RHIDeviceQueueType::Graphics);
    createPass(
        mRenderer.get(), "Reset Counters", RHIDeviceQueueType::Graphics, [=](PassHandle self, Renderer* r)
        { r->BindBufferCopyDst(self, meshletIndirectTasksCtr); }, [=](PassHandle self, Renderer* r, RHICommandList* cmd)
        { cmd->FillBuffer(r->DerefResource(meshletIndirectTasksCtr).Get<RHIBuffer*>(), 0); });
    // Per instance task generation
    createPass(
        mRenderer.get(), "Meshlet Task Generation", RHIDeviceQueueType::Graphics,
        [=](PassHandle self, Renderer* r)
        {
            r->BindShader(self, RHIShaderStageBits::Compute, "taskGeneration", "data/shaders/MVTaskGeneration.spv");
            r->BindBufferUnordered(self, meshletTaskParams, RHIPipelineStageBits::ComputeShader, "tasks");
            r->BindBufferUnordered(self, meshletIndirectTasksCtr, RHIPipelineStageBits::ComputeShader, "counter");
            r->BindBufferStorageRead(self, sceneInstance, RHIPipelineStageBits::ComputeShader, "sceneInstance");
            r->BindBufferStorageRead(self, sceneShared,
                                     RHIPipelineStageBits::ComputeShader | RHIPipelineStageBits::AllGraphics,
                                     "sceneShared");
            r->BindBufferStorageRead(self, sceneConst, RHIPipelineStageBits::ComputeShader, "sceneConst");
            r->BindPushConstant(self, RHIShaderStageBits::Compute, 0, sizeof(uint32_t));
        },
        [=, this](PassHandle self, Renderer* r, RHICommandList* cmd)
        {
            r->CmdSetPipeline(self, cmd);
            r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Compute, 0, mScene->GetParamsAllocationRawOffset());
            r->CmdDispatch(self, cmd, {mScene->mInstanceCount, 1, 1});
        });
    createPass(
        mRenderer.get(), "Background", RHIDeviceQueueType::Graphics,
        [=](PassHandle self, Renderer* r)
        {
            r->BindTextureRTV(
                self, gbuffer,
                {.format = RHIResourceFormat::R8G8B8A8Unorm, .range = RHITextureSubresourceRange::Create()});
            r->BindShader(self, RHIShaderStageBits::Vertex, "vertMain", "data/shaders/VSFullscreen.spv");
            r->BindShader(self, RHIShaderStageBits::Fragment, "fragMain", "data/shaders/MVBackground.spv");
        },
        [=, this](PassHandle self, Renderer* r, RHICommandList* cmd)
        {
            r->CmdBeginGraphics(self, cmd, mViewportSize, {}, {});
            r->CmdSetPipeline(self, cmd);
            cmd->SetViewport(0, 0, mViewportSize.x, mViewportSize.y).SetScissor(0, 0, mViewportSize.x, mViewportSize.y);
            cmd->Draw(3);
            cmd->EndGraphics();
        });
    createPass(
        mRenderer.get(), "Grid View", RHIDeviceQueueType::Graphics,
        [=](PassHandle self, Renderer* r)
        {
            using enum RHIPipelineState::PipelineStateDesc::Attachment::Blending::BlendFactor;
            r->BindShader(self, RHIShaderStageBits::Vertex, "vertMain", "data/shaders/MVGridView.spv");
            r->BindShader(self, RHIShaderStageBits::Fragment, "fragMain", "data/shaders/MVGridView.spv");
            r->BindPushConstant(self, RHIShaderStageBits::Vertex | RHIShaderStageBits::Fragment, 0,
                                sizeof(Grid::Params));
            r->BindTextureRTV(self, gbuffer,
                              {
                                  .format = RHIResourceFormat::R8G8B8A8Unorm,
                                  .range = RHITextureSubresourceRange::Create(),
                              },
                              RHIPipelineState::PipelineStateDesc::Attachment::Blending::GetAlphaBlending());
            r->PassSetRasterizerFlags(self, {.cullMode = RHIPipelineState::PipelineStateDesc::Rasterizer::CullNone});
        },
        [=, this](PassHandle self, Renderer* r, RHICommandList* cmd)
        {
            r->CmdBeginGraphics(self, cmd, mViewportSize, {} /* don't clear RTV */);
            r->CmdSetPipeline(self, cmd);
            r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Vertex | RHIShaderStageBits::Fragment, 0,
                                  mScene->mGrid.GetParams(mScene->mCamera));
            cmd->SetViewport(0, 0, mViewportSize.x, mViewportSize.y).SetScissor(0, 0, mViewportSize.x, mViewportSize.y);
            cmd->Draw(24);
            cmd->EndGraphics();
        });
    createPass(
        mRenderer.get(), "Meshlet Task Submit", RHIDeviceQueueType::Graphics,
        [=](PassHandle self, Renderer* r)
        {
            // Simply fills the dispatch buffer with the number of tasks to dispatch
            r->BindShader(self, RHIShaderStageBits::Compute, "taskSubmit", "data/shaders/MVTaskSubmit.spv");
            r->BindBufferStorageRead(self, meshletIndirectTasksCtr, RHIPipelineStageBits::ComputeShader, "counter");
            r->BindBufferUnordered(self, meshletTaskDispatch, RHIPipelineStageBits::ComputeShader, "dispatch");
        },
        [=](PassHandle self, Renderer* r, RHICommandList* cmd)
        {
            r->CmdSetPipeline(self, cmd);
            r->CmdDispatch(self, cmd, {1, 1, 1});
        });
    createPass(
        mRenderer.get(), "Meshlet Draw", RHIDeviceQueueType::Graphics,
        [=](PassHandle self, Renderer* r)
        {
            r->BindShader(self, RHIShaderStageBits::Task, "meshletTask", "data/shaders/MVMeshletTask.spv");
            r->BindShader(self, RHIShaderStageBits::Mesh, "meshletMesh", "data/shaders/MVMeshletMesh.spv");
            r->BindShader(self, RHIShaderStageBits::Fragment, "meshletFrag", "data/shaders/MVMeshletFrag.spv");
            r->BindBufferShaderRead(self, meshletTaskDispatch,
                                    RHIPipelineStageBits::DrawIndirect | RHIPipelineStageBits::AllGraphics);
            r->BindBufferStorageRead(self, meshletTaskParams, RHIPipelineStageBits::ComputeShader, "tasks");
            r->BindBufferStorageRead(self, meshletIndirectTasksCtr, RHIPipelineStageBits::ComputeShader, "counter");
            r->BindBufferStorageRead(self, sceneInstance, RHIPipelineStageBits::ComputeShader, "sceneInstance");
            r->BindBufferStorageRead(self, sceneShared,
                                     RHIPipelineStageBits::ComputeShader | RHIPipelineStageBits::AllGraphics,
                                     "sceneShared");
            r->BindBufferStorageRead(self, sceneConst, RHIPipelineStageBits::ComputeShader, "sceneConst");
            r->BindPushConstant(self,
                                RHIShaderStageBits::Mesh | RHIShaderStageBits::Task | RHIShaderStageBits::Fragment, 0,
                                sizeof(uint32_t));
            r->BindTextureRTV(self, gbuffer,
                              {
                                  .format = RHIResourceFormat::R8G8B8A8Unorm,
                                  .range = RHITextureSubresourceRange::Create(),
                              });
            r->BindTextureDSV(self, zbuffer,
                              {.format = RHIResourceFormat::D32SignedFloat,
                               .range = RHITextureSubresourceRange::Create(RHITextureAspectFlagBits::Depth)});
        },
        [=, this](PassHandle self, Renderer* r, RHICommandList* cmd)
        {
            auto const& img_wh = mViewportSize;
            r->CmdBeginGraphics(self, cmd, img_wh, {});
            r->CmdSetPipeline(self, cmd);
            r->CmdSetPushConstant(self, cmd,
                                  RHIShaderStageBits::Mesh | RHIShaderStageBits::Task | RHIShaderStageBits::Fragment, 0,
                                  mScene->GetParamsAllocationRawOffset());
            cmd->SetViewport(0, 0, img_wh.x, img_wh.y).SetScissor(0, 0, img_wh.x, img_wh.y);
            cmd->DrawMeshTasksIndirect(r->DerefResource(meshletTaskDispatch).Get<RHIBuffer*>(), 0, 1,
                                       sizeof(MeshletTaskDispatch));
            cmd->EndGraphics();
        });
    // createCSMipGenerationPasses(
    //     mRenderer.get(),"Depth Pyramid", RHIDeviceQueueType::Compute, zbuffer, hiz,
    //     RHITextureAspectFlagBits::Depth, RHIResourceFormat::D32SignedFloat,
    //     RHITextureAspectFlagBits::Color, RHIResourceFormat::R32SignedFloat);
    ImGui_ImplFoundation_CreatePass(mRenderer.get(), "ImGui", true /* clear */,
        [=, this](PassHandle self, Renderer* r)
        {
            // Declare dependency on the GBuffer so it is at correct state
            // when ImGui pass records.
            // The SRV will _not_ be created until @ref Renderer finishes
            // its @ref Setup phase.
            // See @ref ModelViewer::App::OnAfterFrame for further details
            mGBufferSRV = r->BindTextureSRV(
                self, gbuffer, kBindpointIgnored, RHIPipelineStageBits::FragmentShader,
                {
                    .format = RHIResourceFormat::R8G8B8A8Unorm,
                    .range = RHITextureSubresourceRange::Create(),
                });
            // r->BindTextureSRV(
            //     self, hiz, kBindpointIgnored, RHIPipelineStageBits::FragmentShader,
            //     {
            //         .format = RHIResourceFormat::R32SignedFloat,
            //         .range = RHITextureSubresourceRange::Create(RHITextureAspectFlagBits::Color, 0, 13)
            //     });
        });
}
