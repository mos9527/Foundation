#pragma once
// https://levelup.gitconnected.com/organizing-gpu-work-with-directed-acyclic-graphs-f3fd5f2c2af3
#include "../../../Common/Graph.hpp"
#include "../../RHI/Common.hpp"
#include "../../../Common/Task.hpp"

struct RenderContext {};
template<typename T> concept rg_function_compilant = requires(T func, RenderContext& renderContext) { func(renderContext); };

typedef void_function_ptr_type rg_function_ptr;
typedef std::unordered_set<entt::entity> rg_subresources;

class RenderGraph;
struct RenderPass {
	friend class RenderGraph;
private:
	entt::entity rg_entity;
public:
	rg_subresources reads;
	rg_subresources writes;
	rg_subresources readwrites;

	rg_function_ptr executes;

	RenderPass(entt::entity entity) : rg_entity(entity) {};
	RenderPass& write(entt::entity resource) { 		
		writes.insert(resource); 
		return *this; 
	}
	RenderPass& read(entt::entity resource) {
		reads.insert(resource);
		return *this;
	}
	RenderPass& readwrite(entt::entity resource) {
		readwrites.insert(resource);
		return *this;
	}
	template<typename T> RenderPass& execute(T func) requires rg_function_compilant<T> {
		executes = make_task_ptr(func); 
		return *this;
	}
	bool has_dependencies() { return reads.size() > 0 || readwrites.size() > 0; }
	bool reads_from(entt::entity resource) { return reads.contains(resource) || readwrites.contains(resource); }
	bool writes_to(entt::entity resource) { return writes.contains(resource); }
};
class RenderGraph : DAG<entt::entity> {
	std::vector<entt::entity> render_passes;
	std::vector<std::vector<entt::entity>> rg_layers;

	entt::registry& rnd_registry; // registry into renderer's textures, buffers, etc
	entt::registry rg_registry; // local registry for bookkeeping passes
public:
	RenderGraph(entt::registry& _renderer_registry) : rnd_registry(_renderer_registry) {};
	RenderPass& add_pass() {		
		entt::entity rg_entity = rg_registry.create();
		rg_registry.emplace<RenderPass>(rg_entity,rg_entity);
		render_passes.push_back(rg_entity);
		return rg_registry.get<RenderPass>(rg_entity);
	}
	void bulid() {
		reset();
		rg_layers.clear();
		for (entt::entity current : render_passes) {
			auto& pass = rg_registry.get<RenderPass>(current);
			// validation : all writes are on exisiting buffer-like objects
			for (entt::entity entity : pass.writes) {
				auto ptr = rnd_registry.try_get<Tag>(entity); // xxx
				CHECK(ptr);
				CHECK(*ptr == Tag::Texture || *ptr == Tag::Buffer);
			}
			// create adjacency list from read-writes
			if (pass.has_dependencies()) {
				for (entt::entity other : render_passes) {
					if (other != current) {
						auto& other_pass = rg_registry.get<RenderPass>(other);
						for (entt::entity write : other_pass.writes) {
							if (pass.reads_from(write)) {
								add_edge(other, current); 
							}
						}
					}
				}
			}
		}
		auto topsorted = topological_sort();
		auto depths = get_depths(topsorted);
		uint max_depth = 0;
		for (auto& [node, depth] : depths) max_depth = std::max(max_depth, depth);
		rg_layers.resize(max_depth + 1);
		for (auto& [node, depth] : depths) rg_layers[depth].push_back(node);
	}
};