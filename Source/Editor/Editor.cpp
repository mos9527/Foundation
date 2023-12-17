#include "Widgets/Widgets.hpp"
#include "Editor.hpp"
#include "Input/KBMCamera.hpp"
#include "Win32/Win32IO.hpp"
#include "Renderer/Deferred.hpp"
#include "../../Dependencies/IconsFontAwesome6.h"

using namespace RHI;
using namespace EditorGlobals;
#define IMGUI_DEFAULT_FONT  CONCATENATE(SOURCE_DIR,"/../Resources/Fonts/DroidSansFallback.ttf")
#define IMGUI_GLYPH_FONT    CONCATENATE(SOURCE_DIR,"/../Resources/Fonts/fa-solid-900.ttf")
/* ImGui Extern */
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
void Setup_ImGui() {
    // Boilerplate setup
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui_ImplWin32_Init(g_Window);
    static Descriptor ImGuiFontDescriptor = g_RHI.device->GetOnlineDescriptorHeap<DescriptorHeapType::CBV_SRV_UAV>()->AllocatePersistentDescriptor();    
    // We're not expecting to free this
    ImGui_ImplDX12_Init(
        g_RHI.device->GetNativeDevice(),
        RHI_DEFAULT_SWAPCHAIN_BACKBUFFER_COUNT,
        ResourceFormatToD3DFormat(g_RHI.swapchain->GetFormat()),
        *g_RHI.device->GetOnlineDescriptorHeap<DescriptorHeapType::CBV_SRV_UAV>(),
        ImGuiFontDescriptor,
        ImGuiFontDescriptor
    );
    // Display font
    static const float baseFontSize = 16.0f * ImGui_ImplWin32_GetDpiScaleForHwnd(g_Window);
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    io.Fonts->AddFontFromFileTTF(
        IMGUI_DEFAULT_FONT,
        baseFontSize,
        NULL,
        io.Fonts->GetGlyphRangesChineseFull()
    );
    // Glyph font
    static const float iconFontSize = baseFontSize * 2.0f / 3.0f;
    static const ImWchar icons_ranges[] = { ICON_MIN_FA, ICON_MAX_16_FA, 0 };
    ImFontConfig icons_config;
    icons_config.MergeMode = true;
    icons_config.PixelSnapH = true;
    icons_config.GlyphMinAdvanceX = iconFontSize;
    io.Fonts->AddFontFromFileTTF(IMGUI_GLYPH_FONT, iconFontSize, &icons_config, icons_ranges);
}
static KBMCameraController g_cameraController;
void Setup_Scene() {
    auto& camera = g_Scene.scene->graph->emplace_at_root<SceneCameraComponent>();
    camera.set_name("Camera");    
    g_Editor.activeCamera = camera.get_entity();

    auto& light = g_Scene.scene->graph->emplace_at_root<SceneLightComponent>();
    light.set_name("Light");
    light.set_local_transform(AffineTransform::CreateTranslation({0,1.7,0}));
    light.lightType = SceneLightComponent::LightType::AreaDisk;    
    light.intensity = 3.0f;    
    light.color = { 1,1,1,1 };
    g_cameraController.reset();

    g_Editor.editorHDRI = g_Scene.scene->create<AssetHDRIProbeComponent>();
    auto& probe = g_Scene.scene->emplace<AssetHDRIProbeComponent>(g_Editor.editorHDRI);
    probe.setup(g_RHI.device, 512u);
}
void Reset_Scene() {
    g_Scene.scene->reset();
    Setup_Scene();
}
static DefaultTaskThreadPool taskpool;
void Load_Scene(path_t path) {
    CHECK(g_Editor.state != EditorStates::Loading && "Attempted to load new assets while previous load is not finished.");
    g_Editor.importStatus.reset();
    taskpool.push([](path_t filepath) {
        g_Editor.state.transition(EditorEvents::OnLoad);
        UploadContext ctx(g_RHI.device);
        ctx.Begin();
        SceneImporter::load(&ctx, g_Editor.importStatus, *g_Scene.scene, filepath);
        ctx.End().Wait();
        g_Editor.importStatus.uploadComplete = true;
        g_Editor.state.transition(EditorEvents::OnLoadComplete);
        g_Scene.scene->clean<StaticMeshAsset>();
        g_Scene.scene->clean<TextureAsset>();
    }, path);
}
static DeferredRenderer* renderer{};
void EditorWindow::Setup() {
    g_Editor.state.transition(EditorEvents::OnSetup);
    Setup_ImGui();
    Setup_Scene();
    g_cameraController.Win32RawInput_Setup(m_hWnd);    
    renderer = new ::DeferredRenderer(g_RHI.device);
}
void Draw_InvalidState() {}
void Draw_RunningState() {
    OnImGui_ViewportWidget();
    OnImGui_RendererParamsWidget();
    OnImGui_EditorParamsWidget();
    OnImGui_SceneGraphWidget();
    OnImGui_SceneComponentWidget();
}
void Draw_LoadingState() {
    OnImGui_LoadingWidget();
}
void Draw_ImGuiWidgets() {
    switch (g_Editor.state.get_state())
    {
    case EditorStates::Running:
        Draw_RunningState();
        break;
    case EditorStates::Loading:
        Draw_LoadingState();
        break;
    default:
        Draw_InvalidState();
    }
}

void Run_ImGui() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::SetNextWindowViewport(vp->ID);

    ImGuiWindowFlags window_flags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoDocking;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
    window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

    ImGuiID dockspace_id = ImGui::GetID("Editor");
    if (ImGui::Begin("Dockspace", 0, window_flags)) {
        ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), ImGuiDockNodeFlags_None);
        ImGui::PopStyleVar(2);

        if (ImGui::BeginMenuBar()) {
            if (ImGui::MenuItem("Open")) {
                auto pathName = Win32_GetOpenFileNameSimple(g_Window);
                if (pathName.size())
                    Load_Scene(pathName);
            }
            if (ImGui::MenuItem("Reset")) {
                g_RHI.device->Wait();
                Reset_Scene();                
            }            
            ImGui::EndMenuBar();
        }

        Draw_ImGuiWidgets();
        ImGui::End();
    }
}

void Run_UpdateTitle() {
    std::wstring title = std::format(L"Foundation Editor | v" FOUNDATION_VERSION_STRING " | FPS: {}", ImGui::GetIO().Framerate);
    SetWindowText(g_Window, title.c_str());
}
void Run_Update() {
    ZoneScopedN("Editor Update");
    ImGuizmo::BeginFrame();
    Run_ImGui();
}
void EditorWindow::Run() {
    ZoneScopedN("Run Frame");
    g_Editor.state.transition(EditorEvents::OnRunFrame);
    {
        ZoneScopedN("ImGui NewFrame");
        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
    }
    // Editor updates
    Run_Update();
    // Watch shader edits on RenderPass-es    
    CHECK(renderer && "Don't");
    renderer->CheckAndResetPassIfNecessary();
    // Running frames
    ImGui::Render();
    uint bbIndex = g_RHI.swapchain->GetCurrentBackbufferIndex();
    CommandList* gfx = g_RHI.device->GetDefaultCommandList<CommandListType::Direct>();
    TracyD3D12NewFrame(gfx->GetCommandQueue()->TRACY_CTX);

    gfx->ResetAllocator(bbIndex);
    gfx->Begin(bbIndex);

    static ID3D12DescriptorHeap* const heaps[] = { g_RHI.device->GetOnlineDescriptorHeap<DescriptorHeapType::CBV_SRV_UAV>()->GetNativeHeap() };
    gfx->GetNativeCommandList()->SetDescriptorHeaps(1, heaps);
    gfx->GetNativeCommandList()->SetGraphicsRootSignature(*g_RHI.rootSig);
    gfx->GetNativeCommandList()->SetComputeRootSignature(*g_RHI.rootSig);

    if (g_Editor.state == EditorStates::Running) {
        SceneView* sceneView = g_Scene.views[bbIndex];
        auto* camera = g_Scene.scene->try_get<SceneCameraComponent>(g_Editor.activeCamera);
        CHECK(camera && "No camera");
        g_cameraController.update_camera(camera);
        sceneView->Update(
            gfx,
            &g_RHI,
            &g_Scene,
            &g_Editor
        );  
        renderer->Render(sceneView, gfx);
        g_Editor.render.image = (ImTextureID)renderer->r_frameBufferSRV->get_persistent_descriptor().get_gpu_handle().ptr;
        g_Editor.render.materialBuffer = { renderer->r_materialBufferTex, renderer->r_materialBufferSRV };
    }

    gfx->QueueTransitionBarrier(g_RHI.swapchain->GetBackbuffer(bbIndex), ResourceState::RenderTarget);
    gfx->FlushBarriers();        
    gfx->GetNativeCommandList()->OMSetRenderTargets(1, &g_RHI.swapchain->GetBackbufferRTV(bbIndex).get_descriptor().get_cpu_handle(), FALSE, nullptr);
    gfx->BeginEvent(L"ImGui");
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), gfx->GetNativeCommandList());
    gfx->EndEvent();    

    gfx->QueueTransitionBarrier(g_RHI.swapchain->GetBackbuffer(bbIndex), ResourceState::Present);
    gfx->FlushBarriers();
    gfx->Close();
    auto sync = gfx->Execute();    
    {
        ZoneScopedN("Waiting for GPU");
        g_RHI.swapchain->PresentAndMoveToNextFrame(g_Editor.render.vsync);
    }
    FrameMark;
    Run_UpdateTitle();
}

void EditorWindow::Destroy() {
    g_Editor.state.transition(EditorEvents::OnDestroy);
    g_RHI.device->Wait();
    delete renderer;
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    EditorGlobals::DestroyContext();
}

void EditorWindow::WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto resize_swapchain = [&]() {
        g_RHI.swapchain->Resize(std::max((WORD)128u, LOWORD(lParam)), std::max((WORD)128u, HIWORD(lParam)));
    };
    ImGui_ImplWin32_WndProcHandler(hWnd, message, wParam, lParam);
    g_cameraController.Win32_WndProcHandler(hWnd, message, wParam, lParam);
	switch (message)
	{
	case WM_SIZE:
        if (
            g_Editor.state != EditorStates::Uninitialized &&
            g_Editor.state != EditorStates::Setup &&
            g_Editor.state != EditorStates::Destroyed
        ) resize_swapchain();
		break;
	case WM_CLOSE:
        Destroy();
		ExitProcess(0);
		break;
	}
	return;
}