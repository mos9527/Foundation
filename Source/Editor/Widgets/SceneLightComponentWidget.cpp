#include "SceneGraphWidgets.hpp"
#include "AssetWidgets.hpp"

using namespace EditorGlobalContext;
void OnImGui_SceneGraphWidget_SceneLightComponentWidget(SceneLightComponent* light) {	
	ImGui::SliderInt( "Light Type", (int*) & light->lightType, 0, 5);
	ImGui::SliderFloat("Intensity", &light->intensity, 0, 100);	
	ImGui::SliderFloat("Spot/Point: Radius", &light->spot_point_radius, 0, 100);
	ImGui::SliderAngle("Spot: Angle", &light->spot_size_rad,0,360.0f);
	ImGui::SliderFloat("Spot: Blend", &light->spot_size_blend, 0, 1);
	ImGui::DragFloat2( "Area: Extents", (float*)&light->area_quad_disk_extents, 0.01f);	
	ImGui::Checkbox(   "Area: Two Sided", &light->area_quad_disk_twoSided);
	ImGui::SliderFloat("Line: Length", &light->area_line_length,0,10);
	ImGui::SliderFloat("Line: Radius", &light->area_line_radius,0,10);
	ImGui::Checkbox("Line: Caps", &light->area_line_caps);	
	light->update();
}
