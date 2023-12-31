#include "Widgets.hpp"
#include "SceneGraphWidgets.hpp"
#include "../../Dependencies/IconsFontAwesome6.h"

using namespace EditorGlobals;
const char* GetGlyphByType(SceneComponentType type) {
	using enum SceneComponentType;
	switch (type)
	{
	case Collection:
		return ICON_FA_BOX;
	case Camera:
		return ICON_FA_CAMERA;
	case StaticMesh:
		return ICON_FA_CUBE;
	case SkinnedMesh:
		return ICON_FA_PERSON;
	case Light:
		return ICON_FA_LIGHTBULB;
	default:
	case Unknown:
		return ICON_FA_QUESTION;		
	}
}

void OnImGui_SceneComponentWidget() {
	using enum SceneComponentType;
	if (ImGui::Begin("Component")) {
		if (g_Scene.scene->valid<SceneComponentType>(g_Editor.editingComponent)) {
			SceneComponentType type = g_Scene.scene->get_type<SceneComponentType>(g_Editor.editingComponent);
			SceneComponent* componet = g_Scene.scene->get_base<SceneComponent>(g_Editor.editingComponent);
			ImGui::Text("Version: %d", componet->get_version());			
			OnImGui_SceneComponent_TransformWidget(componet);
			switch (type)
			{
				case Collection:
					break;
				case Camera:
					OnImGui_SceneGraphWidget_SceneCameraComponentWidget(static_cast<SceneCameraComponent*>(componet));
					break;
				case StaticMesh:
					OnImGui_SceneGraphWidget_SceneStaticMeshComponentWidget(static_cast<SceneStaticMeshComponent*>(componet));
					break;
				case SkinnedMesh:
					OnImGui_SceneGraphWidget_SceneSkinnedMeshComponentWidget(static_cast<SceneSkinnedMeshComponent*>(componet));
					break;
				case Light:
					OnImGui_SceneGraphWidget_SceneLightComponentWidget(static_cast<SceneLightComponent*>(componet));
					break;
				case Armature:
					OnImGui_SceneGraphWidget_SceneArmatureComponentWidget(static_cast<SceneArmatureComponent*>(componet));
					break;
				default:
				case Unknown:
					break;
			}
		}
	}
	ImGui::End();
}
void OnImGui_SceneGraphWidget() {
	if (ImGui::Begin("Scene")) {
		auto dfs_nodes = [&](auto& func, entt::entity entity, uint depth) -> void {
			SceneComponentType type = g_Scene.scene->get_type<SceneComponentType>(entity);
			SceneComponent* componet = g_Scene.scene->get_base<SceneComponent>(entity);
			uint stack_count = 0;
			ImGuiTreeNodeFlags flags = 0;
			if (g_Editor.editingComponent == entity)
				flags |= ImGuiTreeNodeFlags_Selected;
			size_t id = entt::to_integral(entity);
			ImGui::PushStyleColor(ImGuiCol_Text, componet->get_enabled() ? IM_COL32_WHITE : IM_COL32(127, 127, 127, 127));
			float offset = ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2.5f;
			float cursorX = ImGui::GetCursorPosX();
			ImGui::SetCursorPosX(cursorX + offset);
			ImGui::PushID((void*)id);
			{
				bool enabled = componet->get_enabled();
				ImGui::Checkbox("#", &enabled);
				if (ImGui::IsItemEdited())
					componet->set_enabled(enabled);
			}			
			ImGui::PopID();
			ImGui::SameLine();
			ImGui::SetCursorPosX(cursorX);
			bool hasChild = g_Scene.scene->graph->has_child(entity);
			if (!hasChild) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
			if (ImGui::TreeNodeEx((void*)id, flags, "%s %s", GetGlyphByType(type), componet->get_name())) {
				if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
					g_Editor.editingComponent = entity;					
				}
				ImGui::PushID((void*)id);
				{
					if (ImGui::IsItemHovered() && ImGui::IsMouseDown(ImGuiMouseButton_Right))
						ImGui::OpenPopup("#");
					if (ImGui::BeginPopup("#")) {
						ImGui::SeparatorText("Add...");
						if (ImGui::Selectable("Light")) {
							g_Scene.scene->graph->emplace_child_of<SceneLightComponent>(entity);
						}
						ImGui::EndPopup();
					}
				}
				ImGui::PopID();
				if (hasChild) {
					stack_count++;
					auto& children = g_Scene.scene->graph->child_of(entity);
					std::vector<entt::entity> child_sorted(children.begin(), children.end());
					std::sort(child_sorted.begin(), child_sorted.end(), [&](entt::entity lhs, entt::entity rhs) {
						// sort in lexicographicaly descending order
						int lex = strcmp(g_Scene.scene->get_base<SceneComponent>(lhs)->get_name(), g_Scene.scene->get_base<SceneComponent>(rhs)->get_name());
						if (lex != 0) return lex < 0;
						// same name. to enforce strict weak ordering, entity id is then used instead
						return lhs < rhs;
					});;
					for (auto child : child_sorted)
						func(func, child, depth + 1);
				}
			}
			while (stack_count--) ImGui::TreePop();
			ImGui::PopStyleColor();
		};
		auto root = g_Scene.scene->graph->get_root();
		SceneComponent* componet = g_Scene.scene->get_base<SceneComponent>(root);
		dfs_nodes(dfs_nodes, root, 0);	
	}

	ImGui::End();
}