#include "Widgets/Widgets.hpp"
#include "Editor.hpp"
#include "Input/KBMCamera.hpp"
#include "Win32/Win32IO.hpp"
#include "../../Dependencies/IconsFontAwesome6.h"
using namespace RHI;
using namespace EditorGlobalContext;
#define IMGUI_DEFAULT_FONT "Resources/Fonts/DroidSansFallback.ttf"
#define IMGUI_GLYPH_FONT   "Resources/Fonts/fa-solid-900.ttf"
/* ImGui Extern */
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
void Setup_ImGui() {
    // Boilerplate setup
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui_ImplWin32_Init(window); 
    static Descriptor ImGuiFontDescriptor = device->GetOnlineDescriptorHeap<DescriptorHeapType::CBV_SRV_UAV>()->AllocateDescriptor();    
    ImGui_ImplDX12_Init(
        device->GetNativeDevice(),
        RHI_DEFAULT_SWAPCHAIN_BACKBUFFER_COUNT,
        ResourceFormatToD3DFormat(swapchain->GetFormat()),
        *device->GetOnlineDescriptorHeap<DescriptorHeapType::CBV_SRV_UAV>(),
        ImGuiFontDescriptor,
        ImGuiFontDescriptor
    );
    // Display font
    static const float baseFontSize = 16.0f * ImGui_ImplWin32_GetDpiScaleForHwnd(window);
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
    auto& camera = scene.scene->graph->emplace_at_root<SceneCameraComponent>();
    camera.set_name("Camera");    
    viewport.camera = camera.get_entity();

    auto& light = scene.scene->graph->emplace_at_root<SceneLightComponent>();
    light.set_name("Light");
    light.set_local_transform(AffineTransform::CreateTranslation({0,3,0}));
    light.lightType = SceneLightComponent::LightType::AreaLine;
    light.spot_point_radius = 100.0f;
    light.intensity = 3.0f;
    light.area_line_length = 10.0f;
    light.color = { 1,1,1,1 };
    g_cameraController.reset();
}
void Reset_Scene() {
    scene.scene->reset();    
    Setup_Scene();
}
void Load_Scene(path_t path) {
    CHECK(editor.state != EditorStates::Loading && "Attempted to load new assets while previous load is not finished.");
    editor.importStatus.reset();
    taskpool.push([](path_t filepath) {
        editor.state.transition(EditorEvents::OnLoad);
        UploadContext ctx(device);
        ctx.Begin();
        SceneImporter::load(&ctx, editor.importStatus, *scene.scene, filepath);
        ctx.End().Wait();
        editor.importStatus.uploadComplete = true;
        editor.state.transition(EditorEvents::OnLoadComplete);
        scene.scene->clean<MeshAsset>();
        scene.scene->clean<TextureAsset>();
    }, path);
}

void EditorWindow::Setup() {
    editor.state.transition(EditorEvents::OnSetup);
    Setup_ImGui();
    Setup_Scene();
    g_cameraController.Win32RawInput_Setup(m_hWnd);
}
void Draw_InvalidState() {}
void Draw_RunningState() {
    OnImGui_ViewportWidget();
    OnImGui_RendererWidget();
    OnImGui_SceneGraphWidget();
    OnImGui_SceneComponentWidget();
}
void Draw_LoadingState() {
    OnImGui_LoadingWidget();
}
void Draw_ImGuiWidgets() {
    switch (editor.state.get_state())
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
                auto pathName = Win32_GetOpenFileNameSimple(window);
                if (pathName.size())
                    Load_Scene(pathName);
            }
            if (ImGui::MenuItem("Reset")) {
                device->Wait();
                Reset_Scene();                
            }
            {            
                if (ImGui::MenuItem("Probe"))
                    ImGui::OpenPopup("HDRI Probe");                
                if (ImGui::BeginPopupModal("HDRI Probe")){
                    OnImGui_IBLProbeWidget();
                    ImGui::Separator();
                    if (ImGui::Button("Close"))
                        ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                }                
            }
            ImGui::EndMenuBar();
        }

        Draw_ImGuiWidgets();
        ImGui::End();
    }
}

void Run_UpdateTitle() {
    std::wstring title = std::format(L"Foundation Editor | v" FOUNDATION_VERSION_STRING " | FPS: {}", ImGui::GetIO().Framerate);
    SetWindowText(window, title.c_str());
}

void EditorWindow::Run() {
    std::scoped_lock lock(render.renderMutex);
    editor.state.transition(EditorEvents::OnRunFrame);

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    Run_ImGui();       
    ImGui::Render();
    uint bbIndex = swapchain->GetCurrentBackbufferIndex();
    CommandList* cmd = device->GetDefaultCommandList<CommandListType::Direct>();
    cmd->ResetAllocator(bbIndex);
    cmd->Begin(bbIndex);
    static ID3D12DescriptorHeap* const heaps[] = { device->GetOnlineDescriptorHeap<DescriptorHeapType::CBV_SRV_UAV>()->GetNativeHeap() };
    cmd->GetNativeCommandList()->SetDescriptorHeaps(1, heaps);

    if (editor.state == EditorStates::Running) {
        SceneView* sceneView = scene.views[bbIndex];
        auto* camera = scene.scene->try_get<SceneCameraComponent>(viewport.camera);
        CHECK(camera && "No camera");
        g_cameraController.update_camera(camera);
        sceneView->update(
            *scene.scene,
            *camera,
            SceneView::FrameData{
                .viewportWidth  = std::max(viewport.width,  128u),
                .viewportHeight = std::max(viewport.height, 128u),
                .frameIndex = swapchain->GetFrameIndex(),
                .frameFlags = viewport.frameFlags,
                .backBufferIndex = bbIndex,
                .frameTimePrev = ImGui::GetIO().DeltaTime
            }, 
            SceneView::ShaderData{
                .probe = SceneView::ShaderData::IBLProbeData{
                    .use = editor.iblProbeParam.use,
                    .probe = editor.iblProbe,
                    .diffuseIntensity = editor.iblProbeParam.diffuseIntensity,
                    .specularIntensity = editor.iblProbeParam.specularIntensity,
                    .occlusionStrength = editor.iblProbeParam.occlusionStrength
                },
                .ltcTable = editor.ltcTable
        });        
        render.renderer->Render(sceneView);
    }

    cmd->QueueTransitionBarrier(swapchain->GetBackbuffer(bbIndex), ResourceState::RenderTarget);
    cmd->FlushBarriers();
    cmd->GetNativeCommandList()->OMSetRenderTargets(1, &swapchain->GetBackbufferRTV(bbIndex).descriptor.get_cpu_handle(), FALSE, nullptr);
    PIXBeginEvent(cmd->GetNativeCommandList(), 0, L"ImGui");
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cmd->GetNativeCommandList());
    PIXEndEvent(cmd->GetNativeCommandList());

    cmd->QueueTransitionBarrier(swapchain->GetBackbuffer(bbIndex), ResourceState::Present);
    cmd->FlushBarriers();
    cmd->Close();
    cmd->Execute();
    swapchain->PresentAndMoveToNextFrame(viewport.vsync);

    // Run_UpdateTitle();
}

void EditorWindow::Destroy() {
    editor.state.transition(EditorEvents::OnDestroy);
}

void EditorWindow::WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    auto resize_swapchain = [&]() {
        std::scoped_lock lock(render.renderMutex);
        device->Wait();
        swapchain->Resize(std::max((WORD)128u, LOWORD(lParam)), std::max((WORD)128u, HIWORD(lParam)));
    };
    ImGui_ImplWin32_WndProcHandler(hWnd, message, wParam, lParam);
    g_cameraController.Win32_WndProcHandler(hWnd, message, wParam, lParam);
	switch (message)
	{
	case WM_SIZE:
        if (
            editor.state != EditorStates::Uninitialized &&
            editor.state != EditorStates::Setup &&
            editor.state != EditorStates::Destroyed
        ) resize_swapchain();
		break;
	case WM_CLOSE:
		ExitProcess(0);
		break;
	}
	return;
}