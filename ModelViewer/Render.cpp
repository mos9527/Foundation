#include "App.hpp"
#include "Assets/Mesh.hpp"
using namespace ModelViewer;
const size_t kMaxIndirectCommands = 1e6; // 1 million
void App::OnRendererSetup()
{
    ResourceHandle indirectCommands = createResource(mRenderer.get(), "IndirectCommands",
        RHIBufferDesc{
            .usage = RHIBufferUsageBits::IndirectBuffer | RHIBufferUsageBits::StorageBuffer,
            .size = sizeof(MeshDrawIndirectCmd) * kMaxIndirectCommands
        }
    );
    ResourceHandle counter = createResource(mRenderer.get(), "Command Counter",
        RHIBufferDesc{
            .usage = RHIBufferUsageBits::IndirectBuffer | RHIBufferUsageBits::StorageBuffer | RHIBufferUsageBits::TransferDestination,
            .size = sizeof(int)
        }
    );
    ResourceHandle zbuffer = createResource(mRenderer.get(), "DepthBuffer",
        RHITextureDesc{
            .usage = RHITextureUsageBits::DepthStencil,
            .extent = mRenderer->GetSwapchainExtent3D(),
            .format = RHIResourceFormat::D32SignedFloat,
        }
    );
    ResourceHandle instanceBuffer, metadataBuffer;
    mScene->CreateUpdatePasses(
        mRenderer.get(),
        instanceBuffer,
        metadataBuffer,
        RHIDeviceQueueType::Graphics
    );
    ResourceHandle primitiveBuffer = mRenderer->CreateResource("Primitive", mScene->mPrimitive.Get());
    ResourceHandle vertexBuffer = mRenderer->CreateResource("Vertex", mScene->mVertex.Get());
    ResourceHandle indexBuffer = mRenderer->CreateResource("Index", mScene->mIndex.Get());
    // https://registry.khronos.org/vulkan/specs/latest/html/vkspec.html#drawing-primitive-shading
    // https://registry.khronos.org/vulkan/specs/latest/html/vkspec.html#vkCmdDrawIndexedIndirect
    createPass(mRenderer.get(), "Reset Command Counter", RHIDeviceQueueType::Graphics,
        [=](PassHandle self, Renderer* r)
        {
            r->BindBufferCopyDst(self, counter);
        },
        [=](PassHandle self, Renderer* r, RHICommandList* cmd)
        {
            cmd->FillBuffer(r->DerefResource(counter).Get<RHIBuffer*>(), 0);
        }
    );
    createPass(mRenderer.get(), "Indirect Drawcall Generation [Early]", RHIDeviceQueueType::Graphics,
        [=](PassHandle self, Renderer* r)
        {
            r->BindShader(self, RHIShaderStageBits::Compute, "indirectCullEarly", "data/shaders/MVIndirectCull.spv");
            r->BindBufferUnordered(self, indirectCommands, RHIPipelineStageBits::ComputeShader, "commands");
            r->BindBufferUnordered(self, counter, RHIPipelineStageBits::ComputeShader, "counter");
            r->BindBufferStorage(self, instanceBuffer, RHIPipelineStageBits::ComputeShader, "scInstance");
            r->BindBufferStorage(self, primitiveBuffer, RHIPipelineStageBits::ComputeShader, "scPrimitive");
        },
        [=](PassHandle self, Renderer* r, RHICommandList* cmd)
        {
            r->CmdSetPipeline(self, cmd);
            r->CmdDispatch(self, cmd, { kMaxIndirectCommands, 1, 1 });
        }
    );
    createPass(mRenderer.get(), "Indirect Draw", RHIDeviceQueueType::Graphics,
        [=](PassHandle self, Renderer* r)
        {
            r->BindShader(self, RHIShaderStageBits::Vertex, "vertMain", "data/shaders/MVMeshDraw.spv");
            r->BindShader(self, RHIShaderStageBits::Fragment, "fragMain", "data/shaders/MVMeshDraw.spv");
            r->BindBufferStorage(self, vertexBuffer, RHIPipelineStageBits::VertexShader, "vertices");
            r->BindBufferStorage(self, indexBuffer, RHIPipelineStageBits::VertexShader, "indices");
            r->BindBufferStorage(self, indirectCommands, RHIPipelineStageBits::DrawIndirect | RHIPipelineStageBits::AllGraphics, "commands");
            r->BindBufferStorage(self, instanceBuffer, RHIPipelineStageBits::AllGraphics, "scInstance");
            r->BindBufferShaderRead(self, counter, RHIPipelineStageBits::AllGraphics);
            r->BindVertexInput(self, {.bindings = {{{.stride = sizeof(Vertex)}}}, .attributes = Attributes});
            r->BindPushConstant(self, RHIShaderStageBits::Vertex | RHIShaderStageBits::Fragment, 0, sizeof(DrawPushConstant));
            r->BindBackbufferRTV(self);
            r->BindTextureDSV(self, zbuffer, {
                .format = RHIResourceFormat::D32SignedFloat,
                .range = RHITextureSubresourceRange::Create(RHITextureAspectFlagBits::Depth)
            });
        },
        [=, this](PassHandle self, Renderer* r, RHICommandList* cmd)
        {
            auto const& img_wh = r->GetSwapchainExtent();
            r->CmdBeginGraphics(self, cmd, img_wh);
            r->CmdSetPipeline(self, cmd);
            // Camera matrix
            DrawPushConstant pc {
                .viewProj = mCamera,
                .time = GetApplicationTime<float>()
            };
            r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Vertex | RHIShaderStageBits::Fragment, 0, pc);
            cmd->SetViewport(0, 0, img_wh.x, img_wh.y)
                .SetScissor(0, 0, img_wh.x, img_wh.y);
            // TODO: No VB/IB requires maintenance9 and maintenance6, which are Vulkan 1.4 features
            //       niagara uses those - we'll make do with using an actual VB/IB for now
            cmd->BindVertexBuffer(
                0,
                {{ r->DerefResource(vertexBuffer).Get<RHIBuffer*>() }},
                {{ 0 }}
                )
                .BindIndexBuffer(
                    r->DerefResource(indexBuffer).Get<RHIBuffer*>(),
                    0,
                    RHIResourceFormat::R32Uint)
                .DrawIndexedIndirectCount(
                    r->DerefResource(indirectCommands).Get<RHIBuffer*>(),
                    offsetof(MeshDrawIndirectCmd, indexCount),
                    r->DerefResource(counter).Get<RHIBuffer*>(),
                    0,
                    static_cast<uint32_t>(kMaxIndirectCommands),
                    sizeof(MeshDrawIndirectCmd));
            cmd->EndGraphics();
        }
    );
}