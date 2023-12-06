#include "SceneGraphWidgets.hpp"
#include "AssetWidgets.hpp"

using namespace EditorGlobalContext;
void OnImGui_SceneGraphWidget_SceneArmatureComponentWidget(SceneArmatureComponent* armature) {
	ImGui::SeparatorText("Armature");
	ImGui::Text("Bone Count: %d", armature->boneMap.size());
	ImGui::Text("Keyshape Count: %d", armature->keyShapeMap.size());
}
