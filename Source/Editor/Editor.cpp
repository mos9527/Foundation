#include "Editor.hpp"
#include "../../Dependencies/IconsFontAwesome6.h"

using namespace RHI;
#define IMGUI_DEFAULT_FONT "Resources/Fonts/DroidSansFallback.ttf"
#define IMGUI_GLYPH_FONT   "Resources/Fonts/fa-solid-900.ttf"
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
void Setup_ImGui(EditorWindow* window) {
    // Boilerplate setup
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui_ImplWin32_Init(window->m_hWnd);    
    static Descriptor ImGuiFontDescriptor = EditorGlobalContext::device->GetOnlineDescriptorHeap<DescriptorHeapType::CBV_SRV_UAV>()->AllocateDescriptor();    
    ImGui_ImplDX12_Init(
        EditorGlobalContext::device->GetNativeDevice(),
        RHI_DEFAULT_SWAPCHAIN_BACKBUFFER_COUNT,
        ResourceFormatToD3DFormat(EditorGlobalContext::swapchain->GetFormat()),
        *EditorGlobalContext::device->GetOnlineDescriptorHeap<DescriptorHeapType::CBV_SRV_UAV>(),
        ImGuiFontDescriptor,
        ImGuiFontDescriptor
    );
    // Display font
    static const float baseFontSize = 16.0f * ImGui_ImplWin32_GetDpiScaleForHwnd(window->m_hWnd);
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

void EditorWindow::Setup() {    
    Setup_ImGui(this);
}

void EditorWindow::Run() {
    std::scoped_lock lock(EditorGlobalContext::renderMutex);

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();

    Device* device = EditorGlobalContext::device;
    Swapchain* swapchain = EditorGlobalContext::swapchain;
    CommandList* cmd = device->GetDefaultCommandList<CommandListType::Direct>();
    
    uint bbIndex = swapchain->GetCurrentBackbufferIndex();
    cmd->ResetAllocator(bbIndex);
    cmd->Begin(bbIndex);
    ID3D12DescriptorHeap* const heaps[] = { device->GetOnlineDescriptorHeap<DescriptorHeapType::CBV_SRV_UAV>()->GetNativeHeap() };
    cmd->GetNativeCommandList()->SetDescriptorHeaps(1, heaps);

    SceneView* sceneView = EditorGlobalContext::sceneViews[bbIndex];
    sceneView->update(*EditorGlobalContext::scene, *editorCamera, SceneView::FrameData{
        .viewportWidth  = std::max(viewportWidth, 128u),
        .viewportHeight = std::max(viewportHeight, 128u),
        .frameIndex = swapchain->GetFrameIndex(),
        .frameFlags = rendererFrameFlags,
        .backBufferIndex = bbIndex,
        .frameTimePrev = ImGui::GetIO().DeltaTime
    });
    EditorGlobalContext::renderer->Render(sceneView);
}

void EditorWindow::Destroy() {

}

void EditorWindow::WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    ImGui_ImplWin32_WndProcHandler(hWnd, message, wParam, lParam);
	switch (message)
	{
	case WM_SIZE:
		break;
	case WM_CLOSE:
		ExitProcess(0);
		break;
	}
	return;
}