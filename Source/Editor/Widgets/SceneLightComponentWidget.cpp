#include "SceneGraphWidgets.hpp"
#include "AssetWidgets.hpp"

using namespace EditorGlobalContext;
void OnImGui_SceneGraphWidget_SceneLightComponentWidget(SceneLightComponent* light) {	
	ImGui::SliderInt("Light Type", (int*) & light->lightType, 0, 4);
	ImGui::SliderFloat("Intensity", &light->intensity, 0, 100);	
	ImGui::SliderFloat("Light Radius", &light->radius, 0, 100);	
	ImGui::DragFloat2("Area: Extents", (float*)&light->area_Extents);	
	ImGui::Checkbox("Area: Two Sided", &light->area_TwoSided);	
	ImGui::SliderFloat("Line: Length", &light->line_Length,0,10);
	ImGui::SliderFloat("Line: Radius", &light->line_Radius,0,10);
	ImGui::Checkbox("Line: Caps", &light->line_Caps);
	if (ImGui::IsAnyItemActive())
		light->update();
}
