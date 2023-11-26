#include "Widgets.hpp"
using namespace EditorGlobalContext;
void OnImGui_LoadingWidget() {
	ImGui::SetNextWindowFocus();
	ImGui::SetNextWindowSize(ImGui::GetMainViewport()->WorkSize);
	ImGui::SetNextWindowPos(ImGui::GetMainViewport()->WorkPos);
	if (ImGui::Begin("Loading")) {
		ImGui::ProgressBar((float)editor.importStatus.numUploaded / std::max(editor.importStatus.numToUpload + editor.importStatus.numToUpload, 1u));
		ImGui::End();
	}
}
