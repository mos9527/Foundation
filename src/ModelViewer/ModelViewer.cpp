#include "ModelViewer.hpp"
#include "Scene.hpp"
#include "Mesh.hpp"
/**
 * @brief Model Viewer Applicationw
 */
class ModelViewer : public RenderApplication {
public:
    UniquePtr<Scene> m_scene;
    ResourceHandle m_sceneInstance{kInvalidHandle}, m_scenePrimitive{kInvalidHandle};
    ResourceHandle m_sceneVertex{kInvalidHandle}, m_sceneIndex{kInvalidHandle};
    ResourceHandle m_indirectCommands{kInvalidHandle}, m_counter{kInvalidHandle};
    ResourceHandle m_depthBuffer{kInvalidHandle};
    const size_t kMaxIndirectCommands = 32767;
    /* -- scene -- */
    float4x4 m_camera{ mat4(1.0f) };
    void OnDeviceSetup() override
    {
        m_scene = ConstructUnique<Scene>(
            GetAllocator(),
            m_device.Get(), GetAllocator(),
            m_swapchain->GetImages().size(),
            SceneBudgets{}
        );
    }
    void OnBeforeFrame() override
    {
        m_scene->OnBeforeFrame(m_renderer->GetSync());
    }
    void RendererSetup() override
    {
        m_indirectCommands = createResource(m_renderer.get(), "IndirectCommands",
            RHIBufferDesc{
                .usage = RHIBufferUsageBits::IndirectBuffer | RHIBufferUsageBits::StorageBuffer,
                .size = sizeof(MeshDrawIndirectCmd) * kMaxIndirectCommands
            }
        );
        m_counter = createResource(m_renderer.get(), "Command Counter",
            RHIBufferDesc{
                .usage = RHIBufferUsageBits::IndirectBuffer | RHIBufferUsageBits::StorageBuffer,
                .size = sizeof(int)
            }
        );
        m_depthBuffer = createResource(m_renderer.get(), "DepthBuffer",
            RHITextureDesc{
                .usage = RHITextureUsageBits::DepthStencil,
                .extent = m_renderer->GetSwapchainExtent3D(),
                .format = RHIResourceFormat::D32_SIGNED_FLOAT,
            }
        );
        m_scene->CreateUpdatePasses(
            m_renderer.get(),
            m_sceneInstance, m_scenePrimitive, m_sceneVertex, m_sceneIndex,
            RHIDeviceQueueType::Graphics
        );
        // https://registry.khronos.org/vulkan/specs/latest/html/vkspec.html#drawing-primitive-shading
        // https://registry.khronos.org/vulkan/specs/latest/html/vkspec.html#vkCmdDrawIndexedIndirect
        createPass(m_renderer.get(), "Reset Command Counter", RHIDeviceQueueType::Compute,
            [=, this](PassHandle self, Renderer* r)
            {
                r->BindShader(self, RHIShaderStageBits::Compute, "resetCounter", "data/shaders/MVClearCounters.spv");
                r->BindBufferUnordered(self, m_counter, RHIPipelineStageBits::ComputeShader, "counter");
            },
            [=, this](PassHandle self, Renderer* r, RHICommandList* cmd)
            {
                r->CmdSetPipeline(self, cmd);
                r->CmdDispatch(self, cmd, {1,1,1});
            }
        );
        createPass(m_renderer.get(), "Indirect Drawcall Generation [Early]", RHIDeviceQueueType::Compute,
            [=, this](PassHandle self, Renderer* r)
            {
                r->BindShader(self, RHIShaderStageBits::Compute, "indirectCullEarly", "data/shaders/MVIndirectCull.spv");
                r->BindBufferUnordered(self, m_indirectCommands, RHIPipelineStageBits::ComputeShader, "commands");
                r->BindBufferUnordered(self, m_counter, RHIPipelineStageBits::ComputeShader, "counter");
                r->BindBufferStorage(self, m_sceneInstance, RHIPipelineStageBits::ComputeShader, "scInstance");
                r->BindBufferStorage(self, m_scenePrimitive, RHIPipelineStageBits::ComputeShader, "scPrimitive");
            },
            [=, this](PassHandle self, Renderer* r, RHICommandList* cmd)
            {
                r->CmdSetPipeline(self, cmd);
                r->CmdDispatch(self, cmd, { kMaxIndirectCommands, 1, 1 });
            }
        );
        createPass(m_renderer.get(), "Indirect Draw", RHIDeviceQueueType::Graphics,
            [=, this](PassHandle self, Renderer* r)
            {
                r->BindShader(self, RHIShaderStageBits::Vertex, "vertMain", "data/shaders/MVMeshDraw.spv");
                r->BindShader(self, RHIShaderStageBits::Fragment, "fragMain", "data/shaders/MVMeshDraw.spv");
                r->BindBufferStorage(self, m_sceneVertex, RHIPipelineStageBits::VertexShader, "vertices");
                r->BindBufferStorage(self, m_sceneIndex, RHIPipelineStageBits::VertexShader, "indices");
                r->BindBufferStorage(self, m_indirectCommands, RHIPipelineStageBits::DrawIndirect | RHIPipelineStageBits::AllGraphics, "commands");
                r->BindBufferStorage(self, m_sceneInstance, RHIPipelineStageBits::AllGraphics, "scInstance");
                r->BindBufferShaderRead(self, m_counter, RHIPipelineStageBits::AllGraphics);
                r->BindVertexInput(self, {.bindings = {{{.stride = sizeof(Vertex)}}}, .attributes = Attributes});
                r->BindPushConstant(self, RHIShaderStageBits::Vertex | RHIShaderStageBits::Fragment, 0, sizeof(DrawPushConstant));
                r->BindBackbufferRTV(self);
                r->BindTextureDSV(self, m_depthBuffer, {
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
                // niagara uses this - we'll make do with using an actual VB/IB for now
                cmd->BindVertexBuffer(
                    0,
                    {{
                        r->DerefResource(m_sceneVertex).Get<RHIBuffer*>()
                    }},
                    {{
                        0
                    }})
                    .BindIndexBuffer(
                        r->DerefResource(m_sceneIndex).Get<RHIBuffer*>(),
                        0,
                        RHIResourceFormat::R32_UINT)
                    .DrawIndexedIndirectCount(
                        r->DerefResource(m_indirectCommands).Get<RHIBuffer*>(),
                        offsetof(MeshDrawIndirectCmd, indexCount),
                        r->DerefResource(m_counter).Get<RHIBuffer*>(),
                        0,
                        static_cast<uint32_t>(kMaxIndirectCommands),
                        sizeof(MeshDrawIndirectCmd));
                cmd->EndGraphics();
            }
        );
    }
};

int main(int argc, char** argv) {
    ModelViewer app;
    app.Initialize<VulkanApplication>({ .windowTitle = "Model Viewer", .present = true, .asyncCompute = true });
    Thread render(&ModelViewer::RunForever, &app);
}
