#include <Editor/GPUScene.hpp>
#include <Editor/Mesh.hpp>
#include <RenderUtils/CSDebugText.hpp>
#include "Examples.hpp"
using namespace RenderUtils;
#pragma pack(push, 1)
struct UBO
{
    float4x4 view;
    float4x4 proj;
    float zNear;
    float threshold;
    GSMesh mesh;
};
#pragma pack(pop)
int main()
{
    SDL_Window* window = SDL_CreateWindow("Mesh Shader Hierarchical LOD", 800, 600, Examples_SDLWindowFlagsVulkan);
    UBO ubo{ .threshold = 0.01f};
    CSDebugTextData lines[3]{};
    /* Setup */
    auto [renderer, app, device, swapchain] = Examples_InitVulkan(window);
    auto meshData = device->CreateBuffer({
        .usage = RHIBufferUsageBits::TransferDestination | RHIBufferUsageBits::StorageBuffer,
        .size = 16u * (1u << 20) // 16 MB
    });
    /* Loads and computes DAG LODs and upload immediately */
    {
        FMesh src(GLOBAL_ALLOC);
        LoadObj(src, "data/assets/bunny.obj");
        src.Optimize();
        src.ClusterizeDAG();
        auto staging = device->CreateBuffer(RHIBufferDesc::CreateStagingDesc(meshData->mDesc.size));
        char *ptr = static_cast<char*>(staging->Map()), *dst = ptr;
        auto Write = [&](const void* pData, size_t bytes)
        {
            std::memcpy(dst, pData, bytes);
            uint32_t off = dst - ptr;
            dst += bytes;
            return off;
        };
        auto& mesh = ubo.mesh;
        mesh.vtxCount = src.vertices.size();
        mesh.vtxOffset = Write(src.vertices.data(), sizeof(FVertex) * src.vertices.size());
        // Group data
        mesh.groupCount = src.dag.groups.size();
        mesh.groupOffset = Write(src.dag.groups.data(), sizeof(FLODGroup) * src.dag.groups.size());
        // DAG occupies LOD0 data
        auto& s0 = src.dag;
        mesh.meshletCount = s0.meshlets.size();
        mesh.meshletOffset = Write(s0.meshlets.data(), sizeof(FMeshlet) * s0.meshlets.size());
        mesh.meshletVtxOffset = Write(s0.meshletVtx.data(), sizeof(uint32_t) * s0.meshletVtx.size());
        mesh.meshletTriOffset = Write(s0.meshletTri.data(), sizeof(uint8_t) * s0.meshletTri.size());
        size_t size = dst - ptr;
        dst = ptr;
        ImmediateContext im(RHIDeviceQueueType::Graphics, device.Get());
        im->Begin();
        im->CopyBuffer(staging.Get(), meshData.Get(), {{{.size = size}}});
        im->End();
        // NOTE: No need for transition here.
        // Later WaitIdle ensures there'd be no ROW hazards from this.
        im.Submit();
        im.WaitIdle();
    }
    renderer->BeginSetup();
    auto meshHandle = renderer->CreateResource("Mesh Storage", meshData.Get());
    auto uboHandle = renderer->CreateResource(
        "UBO",
        RHIBufferDesc{.usage = RHIBufferUsageBits::TransferDestination | RHIBufferUsageBits::UniformBuffer,
                      .size = sizeof(UBO)});
    auto zbufferHandle = renderer->CreateResource("ZBuffer",
                                                  RHITextureDesc{.usage = RHITextureUsageBits::DepthStencil,
                                                                 .extent = {4096, 4096, 1},
                                                                 .format = RHIResourceFormat::D32SignedFloat});
    renderer->CreatePass(
        "UBO Update", RHIDeviceQueueType::Graphics, 0u,
        [=](PassHandle self, Renderer* r)
        {
            r->BindBufferCopyDst(self, uboHandle);            
        },
        [&](PassHandle, Renderer* r, RHICommandList* cmd)
        {
            auto* uboData = r->DerefResource(uboHandle).Get<RHIBuffer*>();
            // Possible footgun here - capturing by *value* copies ubo at its
            // initial state.
            // XXX: Figure out how to make this less error-prone.
            cmd->UpdateBuffer(uboData, 0, AsBytes(AsSpan(ubo)));            
        });
    renderer->CreatePass(
        "Mesh Shader", RHIDeviceQueueType::Graphics, 0u,
        [=](PassHandle self, Renderer* r)
        {
            r->BindBackbufferRTV(self);
            r->BindTextureDSV(self, zbufferHandle,
                              {.format = RHIResourceFormat::D32SignedFloat,
                               .range = RHITextureSubresourceRange::Create(RHITextureAspectFlagBits::Depth)});
            // No task shader required. See below.
            r->BindShader(self, RHIShaderStageBits::Mesh, "meshMain", "data/shaders/MeshShaderHierarchicalLODMesh.spv");
            r->BindShader(self, RHIShaderStageBits::Fragment, "fragMain", "data/shaders/MeshShaderBasicFrag.spv");
            // NOTE: globalParams is introduced by slang compiler and is currently not customizable
            //       for uniform storage members
            r->BindBufferUniform(self, uboHandle, RHIPipelineStageBits::MeshShader, "globalParams");
            r->BindBufferStorageRead(self, meshHandle,
                                     RHIPipelineStageBits::MeshShader | RHIPipelineStageBits::FragmentShader, "mesh");
        },
        [&](PassHandle self, Renderer* r, RHICommandList* cmd)
        {
            auto const& img_wh = r->GetSwapchainExtent();
            r->CmdBeginGraphics(self, cmd, img_wh, {{RHIClearColor{}}});
            r->CmdSetPipeline(self, cmd);
            // Simplest dispatch - spawn meshlets one by one to each Mesh Shader WG
            // We don't need a task shader - if unbound, DrawMeshTasks dispatches
            // Mesh Shader workgroups effectively directly.
            cmd->SetViewport(0, 0, img_wh.x, img_wh.y,0, 1, true)
                .SetScissor(0, 0, img_wh.x, img_wh.y)
                .DrawMeshTasks(ubo.mesh.meshletCount, 1, 1)
                .EndGraphics();
        });
    createCSDebugTextPassBackBuffer(renderer, "Debug Text", lines);
    renderer->EndSetup();
    SDL_Event event;
    ExampleFpsCounter fps;
    ExamplesArcballCamera camera{.center = {0, 0.5f, 0}, .radius = 2.0f};
    while (!Examples_ShouldClose(window, renderer, swapchain, &event))
    {
        lines[0].x = 16, lines[0].y = 16, lines[0].SetText(fmt::format("Mesh Shader - Hierarchical LOD FPS: {}", fps.Update()));
        lines[1].x = 16, lines[1].y = 40, lines[1].SetText(fmt::format(ExamplesArcballCamera::kControlsText));
        lines[2].x = 16, lines[2].y = 64, lines[2].SetText(fmt::format("Q,E | LOD Threshold: {}", ubo.threshold));
        if (event.type == SDL_EVENT_KEY_DOWN)
        {
            if (event.key.scancode == SDL_SCANCODE_Q && ubo.threshold > 0.01f)
                ubo.threshold -= 0.01f;
            else if (event.key.scancode == SDL_SCANCODE_E)
                ubo.threshold += 0.01f;
        }
        camera.aspect = swapchain->GetAspectRatio(), camera.fovY = radians(45.0f), camera.zNear = 0.01f;
        camera.Update(event);
        ubo.view = camera.view, ubo.proj = camera.proj, ubo.zNear = camera.zNear;
        Examples_NewFrame(renderer);
    }
    meshData.Release(); // Release - destructs with the device
    Examples_DestroyVulkan(window, renderer, app, device, swapchain);
}
