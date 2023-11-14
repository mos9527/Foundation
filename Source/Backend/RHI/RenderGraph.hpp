#pragma once
// https://levelup.gitconnected.com/organizing-gpu-work-with-directed-acyclic-graphs-f3fd5f2c2af3
#include "../../Common/Graph.hpp"
#include "../../Common/Task.hpp"
#include "../RHI/RHI.hpp"
class RenderGraph;
struct RgContext {
	RenderGraph* graph;
	RHI::CommandList* cmd;
};
template<typename T> concept rg_function_compilant = requires(T func, RgContext& renderContext) { func(renderContext); };
typedef void_function_ptr_type rg_function_ptr;

struct rg_resource {
	enum rg_resource_types {
		Resource,
		ResourceViewSRV,
		ResourceViewUAV,
		ResourceViewRTV,
		ResourceViewDSV
	};
	enum rg_resource_flags {
		Imported,
		Created
	};
	uint version;
	rg_resource_types type;
	rg_resource_flags flag;
	entt::entity resource;
	inline operator entt::entity() const { return resource; }
};
template<> struct std::hash<rg_resource> {
	inline entt::entity operator()(const rg_resource& resource) const { return resource; }
};

typedef std::unordered_set<rg_resource> rg_resources;

class RenderGraph;
struct RenderPass {
	friend class RenderGraph;
private:
	entt::entity rg_entity;
public:
	rg_resources reads;
	rg_resources writes;
	rg_resources readwrites;

	rg_function_ptr executes;

	RenderPass(entt::entity entity) : rg_entity(entity) {};
	RenderPass& write(rg_resource& resource) {
		CHECK(resource.flag == rg_resource::Created);
		resource.version++;
		writes.insert(resource); 
		return *this; 
	}
	RenderPass& readwrite(rg_resource& resource) {
		CHECK(resource.flag == rg_resource::Created);
		resource.version++;
		readwrites.insert(resource);
		return *this;
	}
	RenderPass& read(rg_resource resource) {
		reads.insert(resource);
		return *this;
	}
	template<typename T> RenderPass& execute(T func) requires rg_function_compilant<T> {
		executes = make_task_ptr(func); 
		return *this;
	}
	bool has_dependencies() { return reads.size() > 0 || readwrites.size() > 0; }
	bool reads_from(rg_resource resource) { return reads.contains(resource) || readwrites.contains(resource); }
	bool writes_to(rg_resource resource) { return writes.contains(resource); }
};

typedef RHI::Resource* rg_imported_resource;
typedef RHI::Resource rg_created_resource;

class RenderGraph : DAG<entt::entity> {
	RHI::Device* device;
	entt::registry registry;
	
	std::vector<entt::entity> rnd_passes;
	std::vector<std::vector<entt::entity>> rg_layers;
public:	
	RenderGraph(RHI::Device* device) : device(device) {};
	RenderPass& add_pass() {		
		entt::entity rg_entity = registry.create();
		registry.emplace<RenderPass>(rg_entity,rg_entity);
		rnd_passes.push_back(rg_entity);
		return registry.get<RenderPass>(rg_entity);
	}
	template<typename T> rg_resource create(auto... desc) {
		static_assert(false || "Not implemented.");
	};	
	template<> rg_resource create<RHI::Resource>(RHI::Resource::ResourceDesc desc);
	template<> rg_resource create<RHI::ResourceView>(RHI::Resource* resource);

	template<typename T> rg_resource reference(auto... resource) {
		static_assert(false || "Not implemented.");
	};
	template<> rg_resource reference<RHI::Resource>(RHI::Resource* resource);

	template<typename T> T* resolve(rg_resource resource) { 
		static_assert(false || "Not implemented.");
	};
	template<> RHI::Resource* resolve<RHI::Resource>(rg_resource resource);

	void bulid() {
		reset();
		rg_layers.clear();
		for (entt::entity current : rnd_passes) {
			auto& pass = registry.get<RenderPass>(current);
			// validation : all writes are on exisiting buffer / texture objects
			for (entt::entity entity : pass.writes)
				CHECK(registry.try_get<RHI::Texture>(entity) != nullptr || registry.try_get<RHI::Resource>(entity) != nullptr);
			// create adjacency list from read-writes
			if (pass.has_dependencies()) {
				for (auto& other : rnd_passes) {
					if (other != current) {
						auto& other_pass = registry.get<RenderPass>(other);
						for (auto& write : other_pass.writes) {
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
	void execute(RHI::CommandList* cmd) {
		RgContext context{ .graph = this, .cmd = cmd };

	}
};