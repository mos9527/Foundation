#include "Widgets.hpp"
#include "SceneGraphWidgets.hpp"

using namespace EditorGlobalContext;
const char* GetGlyphByType(SceneComponentType type) {
	using enum SceneComponentType;
	switch (type)
	{
	case Collection:
		return ICON_FA_BOX;
	case Camera:
		return ICON_FA_CAMERA;
	case Mesh:
		return ICON_FA_CUBE;
	case Light:
		return ICON_FA_LIGHTBULB;
	default:
	case Unknown:
		return ICON_FA_QUESTION;		
	}
}

static entt::entity selected;
void OnImGui_SceneComponentWidget() {
	using enum SceneComponentType;
	if (ImGui::Begin("Component")) {
		if (scene.scene->valid<SceneComponentType>(selected)) {
			SceneComponentType type = scene.scene->get_type<SceneComponentType>(selected);
			SceneComponent* componet = scene.scene->get_base<SceneComponent>(selected);
			ImGui::Text("Version: %d", componet->get_version());
			switch (type)
			{
				case Collection:
					break;
				case Camera:
					break;
				case Mesh:
					OnImGui_SceneGraphWidget_SceneMeshComponent(static_cast<SceneMeshComponent*>(componet));
					break;
				case Light:
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
			SceneComponentType type = scene.scene->get_type<SceneComponentType>(entity);
			SceneComponent* componet = scene.scene->get_base<SceneComponent>(entity);
			uint stack_count = 0;
			ImGuiTreeNodeFlags flags = 0;
			if (selected == entity)
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
			bool hasChild = scene.scene->graph->has_child(entity);
			if (!hasChild) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
			if (ImGui::TreeNodeEx((void*)id, flags, "%s %s", GetGlyphByType(type), componet->get_name())) {				
				if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
					selected = entity;
				if (hasChild) {
					stack_count++;
					auto& children = scene.scene->graph->child_of(entity);
					std::vector<entt::entity> child_sorted(children.begin(), children.end());
					std::sort(child_sorted.begin(), child_sorted.end(), [&](entt::entity lhs, entt::entity rhs) {
						// sort in lexicographicaly descending order
						int lex = strcmp(scene.scene->get_base<SceneComponent>(lhs)->get_name(), scene.scene->get_base<SceneComponent>(rhs)->get_name());
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
		auto root = scene.scene->graph->get_root();
		SceneComponent* componet = scene.scene->get_base<SceneComponent>(root);
		dfs_nodes(dfs_nodes, root, 0);
	}
	ImGui::End();
}