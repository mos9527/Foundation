#include "SceneGraphWidgets.hpp"
#include "AssetWidgets.hpp"

using namespace EditorGlobalContext;
void OnImGui_SceneGraphWidget_SceneLightComponentWidget(SceneLightComponent* light) {
	ImGui::SeparatorText("Light");
	float3 direction = light->get_direction_vector();
	float3 translation = light->get_global_translation();
	ImGui::DragFloat3("Direction", (float*)&direction);
	ImGui::DragFloat3("Translation", (float*)&translation);
	if (ImGui::BeginTabBar("##LightProperties")) {
		if (ImGui::BeginTabItem("Point", nullptr, ImGuiTabItemFlags_SetSelected * (light->lightType == SceneLightComponent::Point))) {
			ImGui::ColorEdit3("Color", (float*)&light->color);
			ImGui::SliderFloat("Intensity", &light->intensity, 0, 100);
			ImGui::Separator();
			ImGui::SliderFloat("Radius", &light->spot_point_radius, 0, 100);
			ImGui::EndTabItem();
		}
		if (ImGui::IsItemClicked()) light->lightType = SceneLightComponent::Point;		
		if (ImGui::BeginTabItem("Spot", nullptr, ImGuiTabItemFlags_SetSelected * (light->lightType == SceneLightComponent::Spot))) {		
			ImGui::ColorEdit3("Color", (float*)&light->color);
			ImGui::SliderFloat("Intensity", &light->intensity, 0, 100);
			ImGui::Separator();
			ImGui::SliderFloat("Radius", &light->spot_point_radius, 0, 100);			
			ImGui::SliderAngle("Angle", &light->spot_size_rad, 0, 360.0f);
			ImGui::SliderFloat("Blend", &light->spot_size_blend, 0, 1);
			ImGui::EndTabItem();
		}
		if (ImGui::IsItemClicked()) light->lightType = SceneLightComponent::Spot;		
		if (ImGui::BeginTabItem("Directional", nullptr, ImGuiTabItemFlags_SetSelected * (light->lightType == SceneLightComponent::Directional))) {			
			ImGui::ColorEdit3("Color", (float*)&light->color);
			ImGui::SliderFloat("Intensity", &light->intensity, 0, 100);			
			ImGui::EndTabItem();
		}
		if (ImGui::IsItemClicked()) light->lightType = SceneLightComponent::Directional;		
		if (ImGui::BeginTabItem("Line", nullptr, ImGuiTabItemFlags_SetSelected * (light->lightType == SceneLightComponent::AreaLine))) {			
			ImGui::ColorEdit3("Color", (float*)&light->color);
			ImGui::SliderFloat("Intensity", &light->intensity, 0, 100);
			ImGui::Separator();
			ImGui::SliderFloat("Length", &light->area_line_length, 0, 10);
			ImGui::SliderFloat("Radius", &light->area_line_radius, 0, 10);
			ImGui::Checkbox("With Caps", &light->area_line_caps);
			ImGui::EndTabItem();
		}
		if (ImGui::IsItemClicked()) light->lightType = SceneLightComponent::AreaLine;		
		if (ImGui::BeginTabItem("Disk", nullptr, ImGuiTabItemFlags_SetSelected * (light->lightType == SceneLightComponent::AreaDisk))) {			
			ImGui::ColorEdit3("Color", (float*)&light->color);
			ImGui::SliderFloat("Intensity", &light->intensity, 0, 100);
			ImGui::Separator();
			ImGui::DragFloat2("Extents", (float*)&light->area_quad_disk_extents, 0.01f);
			ImGui::Checkbox("Two Sided", &light->area_quad_disk_twoSided);
			ImGui::EndTabItem();
		}
		if (ImGui::IsItemClicked()) light->lightType = SceneLightComponent::AreaDisk;		
		if (ImGui::BeginTabItem("Quad", nullptr, ImGuiTabItemFlags_SetSelected * (light->lightType == SceneLightComponent::AreaQuad))) {			
			ImGui::ColorEdit3("Color", (float*)&light->color);
			ImGui::SliderFloat("Intensity", &light->intensity, 0, 100);
			ImGui::Separator();
			ImGui::DragFloat2("Extents", (float*)&light->area_quad_disk_extents, 0.01f);
			ImGui::Checkbox("Two Sided", &light->area_quad_disk_twoSided);
			ImGui::EndTabItem();
		}
		if (ImGui::IsItemClicked()) light->lightType = SceneLightComponent::AreaQuad;
		ImGui::EndTabBar();
	}	
	light->update();
}
