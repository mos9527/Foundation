#include "ModelViewer.hpp"
/**
* @brief Model Viewer Application
*/
class ModelViewer : public RenderApplication {
    UniquePtr<Scene> m_scene;
    ResourceHandle m_sceneInstance{kInvalidHandle}, m_scenePrimitive{kInvalidHandle};
    ResourceHandle m_sceneVertex{kInvalidHandle}, m_sceneIndex{kInvalidHandle};
    ResourceHandle m_indirectCommands{kInvalidHandle}, m_counter{kInvalidHandle};

    const size_t kMaxIndirectCommands = 1024;
    void RendererSetup() override
    {
        m_scene = ConstructUnique<Scene>(
        GetAllocator(),
            GetAllocator(),
            m_device.Get(),
            SceneDataDesc{}
        );
        m_scene->CreateUpdatePass(m_renderer.get(),m_sceneInstance, m_scenePrimitive,m_sceneVertex, m_sceneIndex);
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
        createPass(m_renderer.get(), "Indirect Drawcall Generation [Early]", RHIDeviceQueueType::Graphics,
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
    }
};

int main(int argc, char** argv) {
    ModelViewer app;
    app.Initialize<VulkanApplication>({ .windowTitle = "Model Viewer", .present = false, .asyncCompute = true});
    app.RunForever();
}
