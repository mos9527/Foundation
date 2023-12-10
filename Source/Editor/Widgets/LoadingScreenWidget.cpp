#include "Widgets.hpp"
using namespace EditorGlobals;
void OnImGui_LoadingWidget() {
	ImGui::SetNextWindowFocus();
	ImGui::SetNextWindowSize(ImGui::GetMainViewport()->WorkSize);
	ImGui::SetNextWindowPos(ImGui::GetMainViewport()->WorkPos);
	if (ImGui::Begin("Loading")) {
		ImGui::ProgressBar((float)g_Editor.importStatus.numUploaded / std::max(g_Editor.importStatus.numToUpload + g_Editor.importStatus.numToUpload, 1u));
		ImGui::End();
	}
}
