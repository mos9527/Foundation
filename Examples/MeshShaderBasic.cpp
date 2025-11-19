#include <Editor/Mesh.hpp>
#include <Editor/GPUScene.hpp>
#include <RenderUtils/CSDebugText.hpp>
using namespace RenderUtils;
struct UBO
{
    GSMesh mesh;
};
int main()
{
    SDL_Window* window = SDL_CreateWindow("Mesh Shader Basic", 800, 600, Examples_SDLWindowFlagsVulkan);
    CSDebugTextData data{ .x = 16, .y = 16 };
    auto [renderer, app, device, swapchain] = Examples_InitVulkan(window, {});
    auto meshData = device->CreateBuffer({
        .usage = RHIBufferUsageBits::TransferDestination | RHIBufferUsageBits::StorageBuffer,
        .size = 16u * (1u << 20) // 16 MB
    });
    auto uboData = device->CreateBuffer({
        .resource = {.heap = RHIDeviceHeapType::Upload, .hostAccess = RHIResourceHostAccess::WriteOnly },
        .usage = RHIBufferUsageBits::UniformBuffer,
        .size = sizeof(UBO)
    });
    GSMesh mesh;
    {
        // Loads LOD0 and upload immediately
        FMesh src(GLOBAL_ALLOC);
        src.Load("data/assets/bunny.obj");
        src.Clusterize();
        auto staging = device->CreateBuffer(RHIBufferDesc::CreateStagingDesc(meshData->mDesc.size));
        char* ptr = static_cast<char*>(staging->Map()), *dst = ptr;
        auto Write = [&](const void* pData, size_t bytes)
        {
            std::memcpy(dst, pData, bytes);
            uint32_t off = dst - ptr;
            dst += bytes;
            return off;
        };
        mesh.vtxCount = src.vertices.size();
        mesh.vtxOffset = Write(src.vertices.data(), sizeof(GSMesh) * src.vertices.size());
        mesh.lodCount = 1;
        auto& m0 = mesh.lod[0]; auto& s0 = src.lods[0];
        m0.indCount = s0.indices.size();
        m0.indOffset = Write(s0.indices.data(), sizeof(uint32_t) * s0.indices.size());
        m0.meshletCount = s0.meshlets.size();
        m0.meshletOffset = Write(s0.meshlets.data(), sizeof(FMeshlet) * s0.meshlets.size());
        m0.meshletVtxOffset = Write(s0.meshletVtx.data(), sizeof(uint32_t) * s0.meshletVtx.size());
        m0.meshletTriOffset = Write(s0.meshletTri.data(), sizeof(uint8_t) * s0.meshletTri.size());
        size_t size = dst - ptr;
        dst = ptr;
        ImmediateContext im(RHIDeviceQueueType::Graphics, device.Get());
        im->Begin();
        im->BeginTransition();
        im->SetBufferTransition(meshData.Get(), {
            .dstAccess = RHIResourceAccessBits::TransferWrite,
            .dstStage = RHIPipelineStageBits::Transfer,
        });
        im->EndTransition();
        im->CopyBuffer(staging.Get(), meshData.Get(), {{{ .size = size }}});
        im->End();
        // NOTE: No need for another transition here. RG will see to it, and
        // the later WaitIdle ensures there'd be no ROW hazards from this.
        im.Submit();
        im.WaitIdle();
    }
    uboData->Map<UBO>()->mesh = mesh;
    renderer->BeginSetup();
    auto meshHandle = renderer->CreateResource("Mesh Storage", meshData);
    auto uboHandle = renderer->CreateResource("UBO", uboData);
    renderer->CreatePass(
        "Mesh Shader", RHIDeviceQueueType::Graphics, 0u,
        [=](PassHandle self, Renderer* r)
        {
            r->BindBackbufferRTV(self);
            // No task shader required. See below.
            r->BindShader(self, RHIShaderStageBits::Mesh, "meshMain", "data/shaders/MeshShaderBasicMesh.spv");
            r->BindShader(self, RHIShaderStageBits::Fragment, "fragMain", "data/shaders/MeshShaderBasicFrag.spv");
            // NOTE: globalParams is introduced by slang compiler and is currently not customizable
            //       for uniform storage members
            r->BindBufferUniform(self, uboHandle, RHIPipelineStageBits::MeshShader, "globalParams");
            r->BindBufferStorageRead(self, meshHandle, RHIPipelineStageBits::MeshShader | RHIPipelineStageBits::FragmentShader, "mesh");
        },
        [=](PassHandle self, Renderer* r, RHICommandList* cmd)
        {
            auto const& img_wh = r->GetSwapchainExtent();
            r->CmdBeginGraphics(self, cmd, img_wh);
            r->CmdSetPipeline(self, cmd);
            // Simplest dispatch - spawn meshlets one by one to each Mesh Shader WG
            // We don't need a task shader - if unbound, DrawMeshTasks dispatches
            // Mesh Shader workgroups effectively directly.
            cmd->SetViewport(0, 0, img_wh.x, img_wh.y)
                .SetScissor(0, 0, img_wh.x, img_wh.y)
                .DrawMeshTasks(mesh.lod[0].meshletCount, 1, 1)
                .EndGraphics();
        });
    createCSDebugTextPassBackBuffer(renderer, "Debug Text", {data});
    renderer->EndSetup();
    ExampleFpsCounter fps;
    while (!Examples_ShouldClose(window, renderer, swapchain))
    {
        data.SetText(fmt::format("Mesh Shader Basic FPS: {}", fps.Update()));
        Examples_NewFrame(renderer);
    }
    meshData.Release(), uboData.Release();
    Examples_DestroyVulkan(window, renderer, app, device, swapchain);
}
