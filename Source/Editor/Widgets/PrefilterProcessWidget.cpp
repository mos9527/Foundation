#include "Widgets.hpp"
#include "../Win32/Win32IO.hpp"
#include "../../Common/IO.hpp"

#include "../Processor/HDRIProbe.hpp"

using namespace EditorGlobals;

static DefaultTaskThreadPool taskpool;
void OnImGui_IBLProbeWidget() {
	static std::string selectedFile;
	static AssetHandle loadedAsset;		
	ImGui::SetNextWindowSize(ImVec2(512, 512));
	if (ImGui::Button("Open")) {
		auto path = Win32_GetOpenFileNameSimple(g_Window);
		if (path.size()) {			
			selectedFile = wstring_to_utf8(path.c_str());
			g_Editor.state.transition(EditorEvents::OnLoad);
			g_Editor.importStatus.reset();
			loadedAsset = g_Scene.scene->create<TextureAsset>();
			taskpool.push([&](path_t path, AssetHandle handle) -> void {				
				auto image = load_bitmap_RGBA32F(path);
				auto& asset = g_Scene.scene->import_asset(handle, g_RHI.device, &image);
				UploadContext ctx(g_RHI.device);
				ctx.Begin();
				asset.Upload(&ctx);
				ctx.End().Wait();
				g_Editor.state.transition(EditorEvents::OnLoadComplete);				
			}, path, loadedAsset);
		}
	}
	ImGui::SameLine();
	ImGui::Text("File: %s", selectedFile.c_str());
	auto* asset = g_Scene.scene->try_get<TextureAsset>(loadedAsset);
	if (asset && asset->IsUploaded()) {
		ImGui::SeparatorText("Preview");
		float aspect = (float)asset->texture.texture.GetDesc().width / asset->texture.texture.GetDesc().height;
		ImGui::Image((ImTextureID)asset->textureSRV->descriptor.get_gpu_handle().ptr, ImVec2{ 256 * aspect, 256 });
	}
}
