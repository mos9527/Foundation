#include "SceneGraph.hpp"
#include "Scene.hpp"
SceneGraph::SceneGraph(Scene& scene) : scene(scene) {
	root = scene.create<SceneComponent>();
	auto& root_collection = scene.emplace<SceneCollectionComponent>(root);
	root_collection.set_name("Scene Root");
};
Scene& SceneGraph::get_scene() {
	return scene;
}
void SceneGraph::update_parent_child(SceneComponent* parent, SceneComponent* child) {
	if (parent && child) {
		child->version = get_scene().version;
		parent->version = get_scene().version;
		child->globalTransform = parent->globalTransform * child->localTransform;
	}
	else if (child) {
		child->version = get_scene().version;
		child->globalTransform = child->localTransform;
	}
}
void SceneGraph::update(const entt::entity entity, bool associative) {
	scene.version++;	
	get_base(entity)->version = scene.version;
	if (associative)
		reduce(entity, [&](entt::entity current, entt::entity parent) -> void {
			SceneComponent* parentComponent = get_base(parent);
			SceneComponent* childComponent = get_base(current);
			update_parent_child(parentComponent, childComponent);
		});
}
SceneComponent* SceneGraph::get_base(const entt::entity entity) {
	return scene.get_base<SceneComponent>(entity);	
}
#ifdef IMGUI_ENABLED
void SceneGraph::OnImGuiEntityWindow(entt::entity entity) {
		if (!contains(entity)) return;
		SceneComponent* componet = scene.get_base<SceneComponent>(entity);			
		bool isOpen = true;
		if (ImGui::Begin("Component",&isOpen)) {
			ImGui::SeparatorText("Transforms");
			auto transform_ui = [&](const char** names, AffineTransform transform, AffineTransform* outTransform = nullptr) {
				Vector3 translation;
				Quaternion rotationQuaternion;
				Vector3 scale;
				Vector3 eulers;
				transform.Decompose(scale, rotationQuaternion, translation);
				eulers = rotationQuaternion.ToEuler();
				bool hasTranslation = false;
				ImGui::DragFloat3(names[0], (float*)&translation, 0.001f);
				hasTranslation |= ImGui::IsItemActive();
				ImGui::DragFloat3(names[1], (float*)&eulers, XM_PI / 360.0f);
				hasTranslation |= ImGui::IsItemActive();
				ImGui::DragFloat3(names[2], (float*)&scale, 0.01f);
				hasTranslation |= ImGui::IsItemActive();
				rotationQuaternion = rotationQuaternion.CreateFromYawPitchRoll(eulers);
				if (outTransform)
					*outTransform = AffineTransform(translation, rotationQuaternion, scale);
				return hasTranslation;
			};
			ImGui::SeparatorText("Local");
			const char* localNames[3] = { "Local Translation","Local Euler","Local Scale" };
			const char* globalNames[3] = { "Global Translation","Global Euler", "Global Scale" };
			AffineTransform transform;			
			if (transform_ui(localNames, componet->get_local_transform(),&transform))
				componet->set_local_transform(transform);
			ImGui::SeparatorText("Global");
			transform_ui(globalNames, componet->get_global_transform());
			ImGui::SeparatorText("Scene Component");
			componet->OnImGui();
			if (ImGui::IsAnyItemActive())
				componet->update();
			ImGui::End();
		}
}
void SceneGraph::OnImGui() {
	auto dfs_nodes = [&](auto& func, entt::entity entity, uint depth) -> void {
		SceneComponentType tag = scene.get_type<SceneComponentType>(entity);
		SceneComponent* componet = scene.get_base<SceneComponent>(entity);
		uint stack_count = 0;
		ImGuiTreeNodeFlags flags = 0;
		if (ImGui_selected_entity == entity)
			flags |= ImGuiTreeNodeFlags_Selected;
		size_t id = entt::to_integral(entity);
		if (ImGui::TreeNodeEx((void*)id, flags, "%s", componet->get_name())) {
			if (ImGui::IsItemClicked(ImGuiMouseButton_Left))
				ImGui_selected_entity = entity;
			stack_count++;			
			if (has_child(entity)) {
				auto& children = child_of(entity);
				std::vector<entt::entity> child_sorted(children.begin(), children.end());
				std::sort(child_sorted.begin(), child_sorted.end(), [&](entt::entity lhs, entt::entity rhs) {
					// sort in lexicographicaly descending order
					int lex = strcmp(scene.get_base<SceneComponent>(lhs)->get_name(), scene.get_base<SceneComponent>(rhs)->get_name());
					if (lex != 0) return lex < 0;
					// same name. to enforce strict weak ordering, entity id is then used instead
					return lhs < rhs;
				});;
				for (auto child : child_sorted)
					func(func, child, depth + 1);
			}
		}
		while (stack_count--) ImGui::TreePop();
	};
	dfs_nodes(dfs_nodes, root, 0);	
};
#endif