#include "Widgets.hpp"
#include "../Win32/Win32IO.hpp"
#include "../../Common/IO.hpp"
#include "../Renderer/Oneshot.hpp"

using namespace EditorGlobalContext;
void OnImGui_PrefilterProcessWidget() {
	static std::string selectedFile;
	static AssetHandle loadedAsset;
	static std::unique_ptr<OneshotPass<IBLPrefilterPass>> filterPass;
	if (filterPass.get() == nullptr) 
		filterPass = std::make_unique<OneshotPass<IBLPrefilterPass>>(device);
	ImGui::Text("File:%s", selectedFile.c_str());
	ImGui::SameLine();
	if (ImGui::Button("Open")) {
		auto path = Win32_GetOpenFileNameSimple(window);
		if (path.size()) {			
			selectedFile = wstring_to_utf8(path.c_str());
			editor.state.transition(EditorEvents::OnLoad);
			editor.importStatus.reset();
			loadedAsset = scene.scene->create<TextureAsset>();
			taskpool.push([&](path_t path, AssetHandle handle) -> void {				
				auto image = load_bitmap_RGBA32F(path);
				auto& asset = scene.scene->import_asset(handle, device, &image);
				UploadContext ctx(device);
				ctx.Begin();
				asset.Upload(&ctx);
				ctx.Close();
				ctx.Execute().Wait();
				editor.state.transition(EditorEvents::OnLoadComplete);				
			}, path, loadedAsset);
		}
	}
	auto* asset = scene.scene->try_get<TextureAsset>(loadedAsset);
	if (asset && asset->IsUploaded()) {
		ImGui::SeparatorText("Preview");
		float aspect = (float)asset->texture.texture.GetDesc().width / asset->texture.texture.GetDesc().height;
		ImGui::Image((ImTextureID)asset->textureSRV->descriptor.get_gpu_handle().ptr, ImVec2{ 256 * aspect, 256 });
		if (ImGui::Button("Process")) {
			filterPass->Process(asset);
		}
	}
	else {
		if (editor.state == EditorStates::Loading)
			ImGui::Text("Loading...");
	}
}
