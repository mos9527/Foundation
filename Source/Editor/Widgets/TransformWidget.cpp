#include "Widgets.hpp"

void OnImGui_SceneComponent_TransformWidget(SceneComponent* componet) {
	ImGui::SeparatorText("Transforms");
	AffineTransform T = componet->get_local_transform();
	Vector3 scale, translation; Quaternion quat;
	T.Decompose(scale, quat, translation);
	bool transform = false;
	ImGui::DragFloat3("Translation", (float*)&translation, 0.01f);
	transform |= ImGui::IsItemEdited();
	ImGui::DragFloat3("Scale", (float*)&scale, 0.01f);
	transform |= ImGui::IsItemEdited();
	ImGui::DragFloat4("Quaternion", (float*)&quat, 0.01f);
	transform |= ImGui::IsItemEdited();
	Vector3 euler = quat.ToEuler();
	ImGui::SliderFloat3("Euler", (float*)&euler, -XM_PI, XM_PI);
	if (ImGui::IsItemEdited()) {
		quat = Quaternion::CreateFromYawPitchRoll(euler);
		transform |= true;
	}
	if (transform)
		componet->set_local_transform(AffineTransform(translation, quat, scale));
}