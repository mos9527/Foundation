#include "App.hpp"
#include "Mesh.hpp"
using namespace ModelViewer;
const size_t kMaxIndirectCommands = 1e6; // 1 million
void App::OnRendererSetup()
{
    ResourceHandle indirectCommands = createResource(m_renderer.get(), "IndirectCommands",
        RHIBufferDesc{
            .usage = RHIBufferUsageBits::IndirectBuffer | RHIBufferUsageBits::StorageBuffer,
            .size = sizeof(MeshDrawIndirectCmd) * kMaxIndirectCommands
        }
    );
    ResourceHandle counter = createResource(m_renderer.get(), "Command Counter",
        RHIBufferDesc{
            .usage = RHIBufferUsageBits::IndirectBuffer | RHIBufferUsageBits::StorageBuffer,
            .size = sizeof(int)
        }
    );
    ResourceHandle zbuffer = createResource(m_renderer.get(), "DepthBuffer",
        RHITextureDesc{
            .usage = RHITextureUsageBits::DepthStencil,
            .extent = m_renderer->GetSwapchainExtent3D(),
            .format = RHIResourceFormat::D32_SIGNED_FLOAT,
        }
    );
    ResourceHandle instanceBuffer;
    m_scene->CreateInstanceUpdatePass(
        m_renderer.get(),
        instanceBuffer,
        RHIDeviceQueueType::Graphics
    );
    ResourceHandle primitiveBuffer = m_renderer->CreateResource("Primitive", m_scene->m_prmitive.Get());
    ResourceHandle vertexBuffer = m_renderer->CreateResource("Vertex", m_scene->m_vertex.Get());
    ResourceHandle indexBuffer = m_renderer->CreateResource("Index", m_scene->m_index.Get());
    // https://registry.khronos.org/vulkan/specs/latest/html/vkspec.html#drawing-primitive-shading
    // https://registry.khronos.org/vulkan/specs/latest/html/vkspec.html#vkCmdDrawIndexedIndirect
    createPassPriority(m_renderer.get(), "Reset Command Counter", RHIDeviceQueueType::Graphics, 1000,
        [=](PassHandle self, Renderer* r)
        {
            r->BindShader(self, RHIShaderStageBits::Compute, "resetCounter", "data/shaders/MVClearCounters.spv");
            r->BindBufferUnordered(self, counter, RHIPipelineStageBits::ComputeShader, "counter");
        },
        [=](PassHandle self, Renderer* r, RHICommandList* cmd)
        {
            r->CmdSetPipeline(self, cmd);
            r->CmdDispatch(self, cmd, {1,1,1});
        }
    );
    createPass(m_renderer.get(), "Indirect Drawcall Generation [Early]", RHIDeviceQueueType::Graphics,
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
    createPass(m_renderer.get(), "Indirect Draw", RHIDeviceQueueType::Graphics,
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
                .format = RHIResourceFormat::D32_SIGNED_FLOAT,
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
                .viewProj = m_camera,
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
                    RHIResourceFormat::R32_UINT)
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