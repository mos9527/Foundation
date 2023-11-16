#pragma once
// https://levelup.gitconnected.com/organizing-gpu-work-with-directed-acyclic-graphs-f3fd5f2c2af3
#include "../../Common/Graph.hpp"
#include "../../Common/Task.hpp"
#include "../RHI/RHI.hpp"
#include "RenderGraphResource.hpp"

class RenderGraph;
struct RgContext {
	RenderGraph* graph;
	RHI::CommandList* cmd;
};
template<typename T> concept RgFunctionType = requires(T func, RgContext& renderContext) { func(renderContext); };
typedef VoidFunctionPtr RgFunctionPtr;

typedef std::unordered_set<RgHandle> RgResources;

class RenderGraph;
struct RenderPass {
	friend class RenderGraph;
private:
	entt::entity rg_entity;
public:
	// r/w/rws for Created resources
	RgResources reads, writes, readwrites;	
	// function callback for actual rendering work
	RgFunctionPtr executes;

	RenderPass(entt::entity entity) : rg_entity(entity) {};
	RenderPass& write(RgHandle& resource) {
		CHECK(!resource.is_imported());
		resource.version++;
		writes.insert(resource); 
		return *this; 
	}
	RenderPass& readwrite(RgHandle& resource) {
		CHECK(!resource.is_imported());
		resource.version++;
		readwrites.insert(resource);
		return *this;
	}
	RenderPass& read(RgHandle resource) {
		CHECK(!resource.is_imported());
		reads.insert(resource);
		return *this;
	}
	template<typename T> RenderPass& execute(T func) requires RgFunctionType<T> {
		executes = make_task_ptr(func); 
		return *this;
	}
	bool has_execute() { return executes.get() != nullptr; }
	bool has_dependencies() { return reads.size() > 0 || readwrites.size() > 0; }
	bool reads_from(RgHandle resource) { return reads.contains(resource) || readwrites.contains(resource); }
	bool writes_to(RgHandle resource) { return writes.contains(resource); }
};
class RenderGraphResourceCache;
// DAG Graph for managing rendering work
// * RenderGraph is meant to be created / destroyed every frame for execution. Which by itself is pretty cheap.
// * To releive resource creation costs, RenderGraphResourceCache caches them so it can be reused when applicable
class RenderGraph : DAG<entt::entity> {
	friend class RenderGraphResourceCache;
	entt::registry registry;
	RenderGraphResourceCache& cache;

	std::vector<entt::entity> rnd_passes;
	std::vector<std::vector<entt::entity>> rg_layers;
	void build_graph() {
		reset();
		rg_layers.clear();
		for (entt::entity current : rnd_passes) {
			auto& pass = registry.get<RenderPass>(current);
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
		// seperate passes into layers		
		auto topsorted = topological_sort();
		auto depths = get_depths(topsorted);
		uint max_depth = 0;
		for (auto& [node, depth] : depths) max_depth = std::max(max_depth, depth);
		rg_layers.resize(max_depth + 1);
		for (auto& [node, depth] : depths) rg_layers[depth].push_back(node);
	}
public:	
	RenderGraph(RenderGraphResourceCache& cache) : cache(cache) {};
	RenderPass& add_pass() {		
		entt::entity rg_entity = registry.create();
		registry.emplace<RenderPass>(rg_entity,rg_entity);
		rnd_passes.push_back(rg_entity);
		return registry.get<RenderPass>(rg_entity);
	}
	// created resources -> stored as handles
	// note: resource object itself is cached. see RenderGraphResourceCache	
	template<RgDefinedResource T> RgHandle create(RgResourceTraits<T>::desc_type const& desc){
		using traits = RgResourceTraits<T>;
		auto entity = registry.create();
		auto resource = traits::type();		
		resource.desc = desc;
		resource.entity = entity;
		registry.emplace<traits::type>(entity, resource);
		RgHandle handle{
			.version = 0,
			.type = traits::type_enum,
			.flag = RgResourceFlag::Created,
			.entity = entity
		};
		registry.emplace<RgHandle>(entity, handle);
		return handle;
	};
	// imported resources -> stored as pointers
	// imported resoures can only be read.
	template<RgDefinedResource T> RgHandle import(T* resource) {
		auto entity = registry.create();
		registry.emplace<T*>(entity, resource);
		RgHandle handle {
			.version = 0,
			.type = RgResourceTraits<T>::type,
			.flag = RgResourceFlag::Imported,
			.entity = entity
		};
		registry.emplace<RgHandle>(entity, handle);
		return handle;
	};
	// retrives imported RHI object pointer
	// its validity is not guaranteed.
	template<RgDefinedResource T> T* get_imported(RgHandle handle) {
		return registry.get<T*>(handle);
	}
	// retrives created RHI object pointer
	// its validity is guaranteed post cache build
	template<RgDefinedResource T> T* get_created(RgHandle handle);
	// retrives imported / created RHI object pointer
	template<RgDefinedResource T> T* get(RgHandle handle) {
		if (handle.is_imported()) return get_imported<T>(handle);
		return get_created<T>(handle);
	}
	// retrives imported / created RHI object as Resource*
	// if not convertible, nullptr is returned
	// * applicalbe to Buffer & Texture
	RHI::Resource* get_as_resource(RgHandle handle) {
		if (handle.type == RgResourceType::Buffer) return static_cast<RHI::Resource*>(get<RHI::Buffer>(handle));
		if (handle.type == RgResourceType::Texture) return  static_cast<RHI::Resource*>(get<RHI::Texture>(handle));
		return nullptr;
	}
	RenderPass& get_pass(entt::entity pass) {
		return registry.get<RenderPass>(pass);
	}
	RenderPass& get_ending_pass() {
		CHECK(rg_layers.size());
		auto& last_layer = rg_layers.back();
		CHECK(last_layer.size() == 1 || "more than one pass in the ending layer!");
		return get_pass(last_layer.back());
	}	
	void execute(RenderGraphResourceCache& cache, RHI::CommandList* cmd);	
};