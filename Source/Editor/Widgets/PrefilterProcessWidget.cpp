#include "Widgets.hpp"
#include "../Win32/Win32IO.hpp"
#include "../../Common/IO.hpp"

#include "../Processor/IBLProbeProcessor.hpp"

using namespace EditorGlobalContext;
void OnImGui_IBLProbeWidget() {
	static std::string selectedFile;
	static AssetHandle loadedAsset;		
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
				ctx.End().Wait();
				editor.state.transition(EditorEvents::OnLoadComplete);				
			}, path, loadedAsset);
		}
	}
	auto* asset = scene.scene->try_get<TextureAsset>(loadedAsset);
	if (asset && asset->IsUploaded()) {
		ImGui::SeparatorText("Preview");
		float aspect = (float)asset->texture.texture.GetDesc().width / asset->texture.texture.GetDesc().height;
		ImGui::Image((ImTextureID)asset->textureSRV->descriptor.get_gpu_handle().ptr, ImVec2{ 256 * aspect, 256 });
		ImGui::SeparatorText("Processor");
		if (editor.iblProbe->state != IBLProbeProcessorStates::Processing) {
			if (editor.iblProbe->state == IBLProbeProcessorStates::IdleNoProbe)
				ImGui::Text("No probe loaded.");
			if (editor.iblProbe->state == IBLProbeProcessorStates::IdleWithProbe) {
				ImGui::Text("Probe loaded.");
				ImGui::SliderFloat("Diffuse Strength", &editor.iblProbeParam.diffuseIntensity, 0, 1);
				ImGui::SliderFloat("Specular Strength", &editor.iblProbeParam.specularIntensity, 0, 1);
				ImGui::SliderFloat("Occlusion Strength", &editor.iblProbeParam.occlusionStrength, 0, 1);
				if (ImGui::BeginTabBar("##IBLPreview")) {
					if (ImGui::BeginTabItem("Split Sum LUT")) {
						ImGui::Image((ImTextureID)editor.iblProbe->lutArrayUAV->descriptor.get_gpu_handle().ptr, ImVec2{ 128,128 });
						ImGui::EndTabItem();
					}
					if (ImGui::BeginTabItem("Irridance")) {
						ImGui::Image((ImTextureID)editor.iblProbe->irridanceCubeUAV->descriptor.get_gpu_handle().ptr, ImVec2{ 128,128 });
						ImGui::EndTabItem();
					}
					if (ImGui::BeginTabItem("Radiance")) {
						static int mipLevel = 0;
						ImGui::Image((ImTextureID)editor.iblProbe->radianceCubeArrayUAVs[mipLevel]->descriptor.get_gpu_handle().ptr, ImVec2{128,128});
						ImGui::SliderInt("Mip", &mipLevel, 0, editor.iblProbe->numMips - 1);
						ImGui::EndTabItem();
					}
					// xxx add *actual* probe visualizers
					ImGui::EndTabBar();
				}
			}
			if (ImGui::Button("Process")) editor.iblProbe->ProcessAsync(asset);
		}
		else {
			ImGui::Text("Processing...");
			ImGui::Text("%s", editor.iblProbe->state.currentProcess);
			ImGui::ProgressBar((float)editor.iblProbe->state.numProcessed / (editor.iblProbe->state.numProcessed + editor.iblProbe->state.numToProcess));
		}
	}
	else {
		if (editor.state == EditorStates::Loading)
			ImGui::Text("Loading...");
	}
}
