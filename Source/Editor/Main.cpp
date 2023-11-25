#include "../pch.hpp"
#include "ViewportWindow.hpp"


#include "../Runtime/RHI/RHI.hpp"
#include "../Runtime/Asset/AssetRegistry.hpp"
#include "Scene/Scene.hpp"
#include "Scene/SceneView.hpp"
#include "Scene/SceneGraph.hpp"
#include "Scene/SceneImporter.hpp"
#include "Renderer/Deferred.hpp"
using namespace RHI;
#ifdef IMGUI_ENABLED
// Forward declare message handler from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
#endif
int main(int argc, char* argv[]) {    
    FLAGS_alsologtostderr = true;
    google::InitGoogleLogging(argv[0]);
    
    CHECK(SetProcessDPIAware());
    CHECK(SetConsoleOutputCP(65001));

    ViewportWindow vp;
    vp.Create(1600, 1000, L"ViewportWindowClass", L"Viewport");
#ifdef IMGUI_ENABLED
    // SetWindowLongPtr(vp.m_hWnd, GWL_STYLE, WS_THICKFRAME | WS_POPUP);
    // xxx more window controls
    ShowWindow(vp.m_hWnd, SW_SHOW);
#endif
    std::mutex renderMutex;

    RHI::Device device(Device::DeviceDesc{.AdapterIndex = 0});
    RHI::Swapchain swapchain(&device, Swapchain::SwapchainDesc {
        vp.m_hWnd, 1600,1000, ResourceFormat::R8G8B8A8_UNORM
    });

    Scene scene;
    SceneGraph& graph = scene.graph;
    DefaultTaskThreadPool taskpool;   
    DeferredRenderer renderer(&device);
    // Create N-buffered scene views
    std::vector<std::unique_ptr<SceneView>> sceneViews(swapchain.GetBackbufferCount());
    for (auto& view : sceneViews)
        view = std::make_unique<SceneView>(&device);

    SceneCameraComponent& camera = graph.emplace_child_of<SceneCameraComponent>(graph.get_root());    
    camera.set_name("Camera");
    camera.set_local_transform(AffineTransform::CreateTranslation({ 0,0,-20 }));    

    SceneLightComponent& light = graph.emplace_child_of<SceneLightComponent>(graph.get_root());
    light.set_name("Spot Light");
    light.set_local_transform(AffineTransform::CreateFromYawPitchRoll(0, -XM_PIDIV4, XM_PI));
    light.lightType = SceneLightComponent::LightType::Directional;
    light.intensity = 3.0f;    
    light.color = { 1,1,1,1 };
    light.radius = 100.0f;

#ifdef IMGUI_ENABLED
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui_ImplWin32_Init(vp.m_hWnd);
    // Load ImGui resources
    Descriptor ImGuiFontDescriptor = device.GetOnlineDescriptorHeap<DescriptorHeapType::CBV_SRV_UAV>()->AllocateDescriptor();
    ImGui_ImplDX12_Init(device.GetNativeDevice(), RHI_DEFAULT_SWAPCHAIN_BACKBUFFER_COUNT,
        ResourceFormatToD3DFormat(swapchain.GetFormat()), *device.GetOnlineDescriptorHeap<DescriptorHeapType::CBV_SRV_UAV>(),
        ImGuiFontDescriptor,
        ImGuiFontDescriptor
    ); // ImGui won't use more than this one descriptor
    float baseFontSize = 16.0f * ImGui_ImplWin32_GetDpiScaleForHwnd(vp.m_hWnd);    
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    ImGui::GetIO().Fonts->AddFontFromFileTTF(
        "Resources/Fonts/DroidSansFallback.ttf",
        baseFontSize,
        NULL,
        ImGui::GetIO().Fonts->GetGlyphRangesChineseFull()
    );
    ImGui::StyleColorsLight();
    ImGui::GetStyle().WindowPadding = ImVec2(0, 0);
#endif
    vp.SetCallback([&](HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
#ifdef IMGUI_ENABLED
        ImGui_ImplWin32_WndProcHandler(hWnd, message, wParam, lParam);
#endif
        if (message == WM_SIZE) {
            std::scoped_lock lock(renderMutex);
            device.Wait();
            swapchain.Resize(std::max((WORD)128u, LOWORD(lParam)), std::max((WORD)128u, HIWORD(lParam)));
        }
    });

    // Start async upload on the taskpool
    SceneImporter::SceneImporterAtomicStatus uploadStatus;
    taskpool.push([&] {                        
#define GLTF_SAMPLE L"Sponza" // DepthOcclusionTest
        path_t filepath = L"Resources\\glTF-Sample-Models\\2.0\\" GLTF_SAMPLE "\\glTF\\" GLTF_SAMPLE ".glb";        
        UploadContext ctx(&device);
        ctx.Begin();
        SceneImporter::load(&ctx, uploadStatus, scene, filepath);
        ctx.End();
        ctx.Execute().Wait();        
        uploadStatus.uploadComplete = true;
        scene.clean<MeshAsset>();
        scene.clean<TextureAsset>();
    });

    bool vsync = false;
    auto cmd = device.GetDefaultCommandList<CommandListType::Direct>();
    auto render = [&]() {
        std::scoped_lock lock(renderMutex);        
#ifdef IMGUI_ENABLED
        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();        
#endif
        uint bbIndex = swapchain.GetCurrentBackbufferIndex();
        cmd->ResetAllocator(bbIndex);
        cmd->Begin(bbIndex);
        /* FRAME BEGIN */
        ID3D12DescriptorHeap* const heaps[] = { device.GetOnlineDescriptorHeap<DescriptorHeapType::CBV_SRV_UAV>()->GetNativeHeap() };
        cmd->GetNativeCommandList()->SetDescriptorHeaps(1, heaps);
#ifdef IMGUI_ENABLED
        {
            const ImGuiViewport* viewport = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(viewport->WorkPos);
            ImGui::SetNextWindowSize(viewport->WorkSize);
            ImGui::SetNextWindowViewport(viewport->ID);
        }
        ImGuiWindowFlags window_flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
        window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;        
        ImGui::Begin("Dockspace",0, window_flags);
        ImGuiID dockspace_id = ImGui::GetID("Editor");
        ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);
        ImGui::PopStyleVar(2);
        if (ImGui::BeginMenuBar()) {
            static POINT offset; 
            static bool dragging = false;
            if (ImGui::IsMouseHoveringRect(ImGui::GetCurrentWindow()->MenuBarRect().Min, ImGui::GetCurrentWindow()->MenuBarRect().Max)) { // enable window movements with menu bar
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    POINT cursorPos;
                    GetCursorPos(&cursorPos);

                    RECT rect;
                    GetWindowRect(vp.m_hWnd, &rect);

                    offset.x = cursorPos.x - rect.left;
                    offset.y = cursorPos.y - rect.top;
                    dragging = true;
                }
            }
            if (dragging) {
                POINT cursorPos;
                GetCursorPos(&cursorPos);
                SetWindowPos(vp.m_hWnd, NULL, cursorPos.x - offset.x, cursorPos.y - offset.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
            }            
            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                dragging = false;
            }
            ImGui::Text("Foundation Editor | v" FOUNDATION_VERSION_STRING " | FPS: % .3f", ImGui::GetIO().Framerate);
            ImGui::EndMenuBar();
        }
#endif
        uint viewportWidth = swapchain.GetWidth(), viewportHeight = swapchain.GetHeight();
#ifdef IMGUI_ENABLED
        bool viewport_Begin = ImGui::Begin("Viewport", 0, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        auto viewportSize = (ImGui::GetWindowContentRegionMax() - ImGui::GetWindowContentRegionMin());
        viewportWidth = viewportSize.x, viewportHeight = viewportSize.y;
#endif
#ifdef IMGUI_ENABLED        
        if (uploadStatus.uploadComplete && ImGui::Begin("Scene Graph")) {
            graph.OnImGui();
            graph.OnImGuiEntityWindow(graph.ImGui_selected_entity);
            ImGui::End();
        }
#else
        scene.update();
#endif
        UINT frameFlags = 0;
        static double debug_RenderCPUTime = 0.0f;
#ifdef IMGUI_ENABLED
        if (ImGui::Begin("Renderer")) {
            ImGui::Text("RenderGraph CPU Time : %3lf ms", debug_RenderCPUTime);
            ImGui::Text("Renderer    Frametime: %3f ms", ImGui::GetIO().DeltaTime * 1000);
            static bool debug_ViewAlbedo = false, debug_ViewLod = false, debug_Wireframe = false;
            static bool debug_FrustumCull = true, debug_OcclusionCull = true;
            ImGui::Checkbox("View LOD", &debug_ViewLod);
            ImGui::Checkbox("View Albedo", &debug_ViewAlbedo);
            ImGui::Checkbox("Wireframe", &debug_Wireframe);
            ImGui::Checkbox("Frustum Cull", &debug_FrustumCull);
            ImGui::Checkbox("Occlusion Cull", &debug_OcclusionCull);
            if (debug_ViewAlbedo) frameFlags |= FRAME_FLAG_DEBUG_VIEW_ALBEDO;
            if (debug_ViewLod) frameFlags |= FRAME_FLAG_DEBUG_VIEW_LOD;
            if (debug_Wireframe) frameFlags |= FRAME_FLAG_WIREFRAME;
            if (debug_FrustumCull) frameFlags |= FRAME_FLAG_FRUSTUM_CULL;
            if (debug_OcclusionCull) frameFlags |= FRAME_FLAG_OCCLUSION_CULL;
            ImGui::End();
        }
#endif
        static ShaderResourceView* pSrv;
        if (uploadStatus.uploadComplete) {
            /* RENDER BEGIN */
            bool needRefresh = sceneViews[bbIndex]->update(scene, camera, SceneView::FrameData{
                    .viewportWidth = std::max(viewportWidth,128u),
                    .viewportHeight = std::max(viewportHeight, 128u),
                    .frameIndex = swapchain.GetFrameIndex(),
                    .frameFlags = frameFlags,
                    .backBufferIndex = bbIndex,
                    .frameTimePrev = ImGui::GetIO().DeltaTime
            });
            double begin = hires_millis();
            pSrv = renderer.Render(sceneViews[bbIndex].get());
            debug_RenderCPUTime = hires_millis() - begin;            
            /* RENDER END */
        }
        else {
#ifdef IMGUI_ENABLED
            ImGui::Text("Loading... %d/%d", uploadStatus.numUploaded.load(), uploadStatus.numToUpload.load());
#endif
        }
        cmd->Barrier(swapchain.GetBackbuffer(bbIndex), ResourceState::RenderTarget);
        cmd->FlushBarriers();
        auto& rtv = swapchain.GetBackbufferRTV(bbIndex);
        const float clearColor[] = { 0.0f, 0.2f, 0.4f, 1.0f };
        cmd->GetNativeCommandList()->ClearRenderTargetView(rtv.descriptor.get_cpu_handle(), clearColor, 0, nullptr);
        cmd->GetNativeCommandList()->OMSetRenderTargets(1, &rtv.descriptor.get_cpu_handle(), FALSE, nullptr);
#ifdef IMGUI_ENABLED
        if (pSrv) 
            ImGui::Image((ImTextureID)pSrv->descriptor.get_gpu_handle().ptr, viewportSize);
        if (viewport_Begin) ImGui::End(); // Viewport
#else        
        // xxx blit image to backbuffer
#endif
#ifdef IMGUI_ENABLED
        ImGui::End(); // DockSpace
        ImGui::Render();
        PIXBeginEvent(cmd->GetNativeCommandList(), 0, L"ImGui");
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cmd->GetNativeCommandList());
        PIXEndEvent(cmd->GetNativeCommandList());
#endif
        /* FRAME END */
        cmd->Barrier(swapchain.GetBackbuffer(bbIndex), ResourceState::Present);
        cmd->FlushBarriers();
        cmd->Close();
        cmd->Execute();
        swapchain.PresentAndMoveToNextFrame(vsync);
    };

    // win32 message pump
    MSG  msg{};
    while (1) {
        if (PeekMessage(&msg, NULL, 0U, 0U, PM_REMOVE) != 0)
        {
            // Translate and dispatch the message
            TranslateMessage(&msg);
            DispatchMessage(&msg);            
        }
        else {
            // Render!
            render();
        }
    }
}