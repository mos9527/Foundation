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
void SceneGraph::update(const entt::entity entity) {
	scene.version++;
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
void SceneGraph::OnImGui() {
	auto dfs_nodes = [&](auto& func, entt::entity entity, uint depth) -> void {
		SceneComponentType tag = scene.get_type<SceneComponentType>(entity);
		SceneComponent* componet = scene.get_base<SceneComponent>(entity);
		uint stack_count = 0;
		if (ImGui::TreeNode(componet->get_name())) {
			ImGui::SeparatorText("Transforms");
			auto transform_ui = [&](const char** names, AffineTransform transform) {
				Vector3 translation;
				Quaternion rotationQuaternion;
				Vector3 scale;
				Vector3 eulers;
				transform.Decompose(scale, rotationQuaternion, translation);
				eulers = rotationQuaternion.ToEuler();
				ImGui::DragFloat3(names[0], (float*)&translation, 0.001f);
				ImGui::DragFloat3(names[1], (float*)&eulers, XM_PI / 360.0f);
				ImGui::DragFloat3(names[2], (float*)&scale, 0.01f);
				rotationQuaternion = rotationQuaternion.CreateFromYawPitchRoll(eulers);
				return AffineTransform(translation, rotationQuaternion, scale);
			};
			ImGui::SeparatorText("Local");
			const char* localNames[3] = { "Local Translation","Local Euler","Local Scale" };
			const char* globalNames[3] = { "Global Translation","Global Euler", "Global Scale" };			
			componet->set_local_transform(transform_ui(localNames, componet->localTransform));
			ImGui::SeparatorText("Global");
			transform_ui(globalNames, componet->get_global_transform());
			ImGui::SeparatorText("Scene Component");
			componet->OnImGui();

			stack_count++;
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
		while (stack_count--) ImGui::TreePop();
	};
	dfs_nodes(dfs_nodes, root, 0);
};
#endif