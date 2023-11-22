#include "../pch.hpp"
#include "ViewportWindow.hpp"

#include "../Runtime/RHI/RHI.hpp"
#include "../Runtime/AssetRegistry/AssetRegistry.hpp"
#include "../Runtime/SceneGraph/SceneGraph.hpp"
#include "../Runtime/SceneGraph/SceneGraphView.hpp"

#include "../Runtime/Renderer/Deferred.hpp"
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

    DefaultTaskThreadPool taskpool;
    AssetRegistry assets;
    SceneGraph scene{ assets };
    std::vector<SceneGraphView> sceneViews;
    for (int i=0;i < swapchain.GetBackbufferCount();i++) 
        sceneViews.emplace_back(&device);
    DeferredRenderer renderer(&device);
    
    scene.set_active_camera(scene.create_child_of<CameraComponent>(scene.get_root()));
    auto& camera = scene.get_active_camera();
    camera.set_name("Camera");
    camera.localTransform = AffineTransform::CreateTranslation({ 0,0,-20 });

    auto lightEntity = scene.create_child_of<LightComponent>(scene.get_root());
    auto& light = scene.get<LightComponent>(lightEntity);
    light.set_name("Spot Light");
    light.localTransform = AffineTransform::CreateFromYawPitchRoll(0,-XM_PIDIV4,XM_PI);
    light.type = LightComponent::LightType::Directional;
    light.intensity = 3.0f;    
    light.color = { 1,1,1,1 };
    light.radius = 100.0f;

#ifdef IMGUI_ENABLED
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui_ImplWin32_Init(vp.m_hWnd);

    Descriptor ImGuiFontDescriptor = device.GetOnlineDescriptorHeap<DescriptorHeapType::CBV_SRV_UAV>()->AllocateDescriptor();
    ImGui_ImplDX12_Init(device.GetNativeDevice(), RHI_DEFAULT_SWAPCHAIN_BACKBUFFER_COUNT,
        ResourceFormatToD3DFormat(swapchain.GetFormat()), *device.GetOnlineDescriptorHeap<DescriptorHeapType::CBV_SRV_UAV>(),
        ImGuiFontDescriptor,
        ImGuiFontDescriptor
    ); // ImGui won't use more than this one descriptor
    float baseFontSize = 16.0f * ImGui_ImplWin32_GetDpiScaleForHwnd(vp.m_hWnd);    
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    ImGui::GetIO().Fonts->AddFontFromFileTTF(
        "../Resources/Fonts/DroidSansFallback.ttf",
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

    SyncFence upload;
    taskpool.push([&] {        
        LOG(INFO) << "Loading scene";
        Assimp::Importer importer;
#define GLTF_SAMPLE L"Sponza" // DepthOcclusionTest
        path_t filepath = L"..\\Resources\\glTF-Sample-Models\\2.0\\" GLTF_SAMPLE "\\glTF\\" GLTF_SAMPLE ".gltf";
        std::string u8path = (const char*)filepath.u8string().c_str();
        auto imported = importer.ReadFile(u8path, aiProcess_Triangulate | aiProcess_ConvertToLeftHanded | aiProcess_CalcTangentSpace | aiProcess_SplitLargeMeshes);
        scene.load_from_aiScene(imported, filepath.parent_path());        
        LOG(INFO) << "Requesting upload";
        CommandList* cmd = device.GetCommandList<CommandListType::Copy>();
        device.BeginUpload(cmd);
        assets.upload_all<StaticMeshAsset>(&device);
        assets.upload_all<SDRImageAsset>(&device);
        upload = device.EndUpload();
    });
    // xxx cleaning

    bool vsync = false;
    auto cmd = device.GetCommandList<CommandListType::Direct>();
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
        if (ImGui::Begin("Scene Graph")) {
            scene.OnImGui();
            scene.update();
            ImGui::End();
        }
#else
        scene.update();
#endif
        UINT frameFlags = 0;
#ifdef IMGUI_ENABLED
        if (ImGui::Begin("Renderer")) {
            static bool debug_ViewAlbedo = false, debug_ViewLod = false, debug_Wireframe = false;
            static bool debug_FrustumCull = true, debug_OcclusionCull = true;
            ImGui::Checkbox("View LOD", &debug_ViewLod);
            ImGui::Checkbox("View Albedo", &debug_ViewAlbedo);
            ImGui::Checkbox("Wireframe", &debug_Wireframe);
            ImGui::Checkbox("Frustum Cull", &debug_FrustumCull);
            ImGui::Checkbox("Occlusion Cull", &debug_OcclusionCull);
            if (debug_ViewAlbedo) frameFlags |= FRAME_FLAG_VIEW_ALBEDO;
            if (debug_ViewLod) frameFlags |= FRAME_FLAG_DEBUG_VIEW_LOD;
            if (debug_Wireframe) frameFlags |= FRAME_FLAG_WIREFRAME;
            if (debug_FrustumCull) frameFlags |= FRAME_FLAG_FRUSTUM_CULL;
            if (debug_OcclusionCull) frameFlags |= FRAME_FLAG_OCCLUSION_CULL;
            ImGui::End();
        }
#endif
        ShaderResourceView* pSrv = nullptr;
        if (upload.IsCompleted()) {
            sceneViews[bbIndex].update(SceneGraphView::FrameData{
                .scene = &scene,
                .viewportWidth = viewportWidth,
                .viewportHeight = viewportHeight,
                .frameIndex = swapchain.GetFrameIndex(),
                .frameFlags = frameFlags,
                .backBufferIndex = bbIndex,
                .frameTimePrev = ImGui::GetIO().DeltaTime
            });
            pSrv = renderer.Render(&sceneViews[bbIndex]);
        }
        else {
#ifdef IMGUI_ENABLED
            ImGui::Text("Loading...");
#endif
        }
        swapchain.GetBackbuffer(bbIndex)->SetBarrier(cmd, ResourceState::RenderTarget);
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
        swapchain.GetBackbuffer(bbIndex)->SetBarrier(cmd, ResourceState::Present);
        cmd->End();
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