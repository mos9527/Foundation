#pragma once
// https://levelup.gitconnected.com/organizing-gpu-work-with-directed-acyclic-graphs-f3fd5f2c2af3
#include "../../Common/Graph.hpp"
#include "../../Common/Task.hpp"
#include "../../Common/Allocator.hpp"
#include "../RHI/RHI.hpp"
#include "RenderGraphResource.hpp"
#include "RenderGraphResourceCache.hpp"
class RenderGraph;
struct RgContext {
	RenderGraph* graph;
	RenderGraphResourceCache* cache;
	RHI::CommandList* cmd;
};

typedef unordered_DAG<entt::entity, entt::entity> graph_type;
typedef std::function<void(RgContext&)> RgFunction;
typedef std::unordered_set<RgHandle> RgResources;
typedef std::vector<entt::entity> RgRndPasses;

class RenderGraph;
struct RenderGraphPass {
	friend class RenderGraph;
private:
	entt::entity rg_entity;
	RgResources reads, writes, readwrites;
	// r/w/rws for Created resources
	RgFunction executes;
	// function callback for actual rendering work
	static inline bool check_handle(RgHandle& resource) {
		return (resource.type == RgResourceType::Texture || resource.type == RgResourceType::Buffer || resource.type == RgResourceType::Dummy);
	}
public:
	const wchar_t* name = nullptr;
	RenderGraphPass(entt::entity entity, const wchar_t* name) : rg_entity(entity), name(name) {};
	RenderGraphPass& write(RgHandle& resource) {
		CHECK(check_handle(resource));
		resource.version++;
		writes.insert(resource); 
		return *this; 
	}
	RenderGraphPass& readwrite(RgHandle& resource) {
		CHECK(check_handle(resource));
		resource.version++;
		readwrites.insert(resource);
		return *this;
	}
	RenderGraphPass& read(RgHandle& resource) {
		CHECK(check_handle(resource));
		reads.insert(resource);
		return *this;
	}
    RenderGraphPass& execute(RgFunction&& func) {		
		executes = std::move(func);
		return *this;
	}
	bool has_execute() { return executes != nullptr; }
	bool has_dependencies() { return reads.size() > 0; }
	bool reads_from(RgHandle resource) { return reads.contains(resource); }
	bool writes_to(RgHandle resource) { return writes.contains(resource) || readwrites.contains(resource); }
};

// DAG Graph for managing rendering work
// * RenderGraph handles (RgHandles) has deterministic hash value (i.e. entity value) per RenderGraph instance
// * Thus, RenderGraph is meant to be created / destroyed every frame for execution. Which by itself is pretty cheap.
// * To releive resource creation costs, RenderGraphResourceCache caches them so it can be reused when applicable
// xxx Optionaly support baking command bundles?
class RenderGraph : graph_type {	
	friend class RenderGraphResourceCache;	
	RenderGraphResourceCache& cache;	
	entt::registry registry;		
	entt::entity epiloguePass;
	
	RgRndPasses passes;
	std::vector<RgRndPasses> layers;	

	void build_graph() {
		layers.clear();
		for (entt::entity current : passes) {
			auto& pass = registry.get<RenderGraphPass>(current);
			// create adjacency list from read-writes
			if (pass.has_dependencies()) {
				for (auto& other : passes) {
					if (other != current) {
						auto& other_pass = registry.get<RenderGraphPass>(other);
						for (auto& write : other_pass.writes) {
							if (pass.reads_from(write)) {
								add_edge(other, current);
							}
						}
						for (auto& readwrite : other_pass.readwrites) {
							if (pass.reads_from(readwrite)) {
								add_edge(other, current);
							}
						}
					}
				}
			}
		}
		// seperate passes into layers		
		auto topsorted = topological_sort();
		if (!topsorted.size() && graph.size()) {
			// cyclic dependency
			LOG(ERROR) << "Cyclic dependency in RenderGraph. Consider using import().";
			LOG(INFO) << "Current Dependency Graph";
			set_type visited;
			auto dfs_nodes = [&](auto& func, vertex_type current, uint depth) -> void {
				auto& pass = registry.get<RenderGraphPass>(current);
				std::string prefix; for (uint i = 0; i < depth; i++) prefix += '\t';
				LOG(INFO) << prefix << wstring_to_utf8(pass.name);
				if (visited.contains(current)) {
					LOG(ERROR) << prefix << " ^ Potential loop here.";
					return;
				}
				visited.insert(current);
				for (auto child : graph[current])
					func(func, child, depth+1);
			};
			for (auto& [vtx, tree] : graph)
				dfs_nodes(dfs_nodes, vtx, 0);
#ifdef _DEBUG
			__debugbreak();
#endif
		}		
		auto depths = get_depths(topsorted);
		uint max_depth = 0;
		for (auto& [node, depth] : depths) max_depth = std::max(max_depth, depth);
		layers.resize(max_depth + 1);
		for (auto& [node, depth] : depths) layers[depth].push_back(node);
	}
public:	
	RenderGraph(RenderGraphResourceCache& cache) : cache(cache) {
		epiloguePass = registry.create();
		registry.emplace<RenderGraphPass>(epiloguePass, epiloguePass, L"Epilogue");
		passes.push_back(epiloguePass);
	};
	RenderGraphPass& add_pass(const wchar_t* name=L"<unamed>") {
		entt::entity rg_entity = registry.create();
		passes.push_back(rg_entity);
		return registry.emplace<RenderGraphPass>(rg_entity,rg_entity,name);		
	}
	// created resources -> stored as handles
	// note: resource object itself is cached. see RenderGraphResourceCache	
	template<RgDefinedResource T> RgHandle& create(RgResourceTraits<T>::desc_type const& desc){
		using traits = RgResourceTraits<T>;
		auto entity = registry.create();
		auto resource = traits::resource_type();
		resource.desc = desc;
		resource.entity = entity;
		registry.emplace<typename traits::resource_type>(entity, resource);
		RgHandle handle{
			.version = 0,
			.type = traits::type_enum,			
			.entity = entity
		};		
		return registry.emplace<RgHandle>(entity, handle);;
	};
	// retrives imported / created RHI object pointer
	template<RgDefinedResource T> T* get(RgHandle handle) {		
		return cache.get<T>(handle);
	}
	// retrives imported / created RHI object as Resource*
	// if not convertible, nullptr is returned
	// * applicalbe to Buffer & Texture
	RHI::Resource* get_as_resource(RgHandle handle) {
		if (handle.type == RgResourceType::Buffer) return static_cast<RHI::Resource*>(get<RHI::Buffer>(handle));
		if (handle.type == RgResourceType::Texture) return static_cast<RHI::Resource*>(get<RHI::Texture>(handle));
		return nullptr;
	}
	RenderGraphPass& get_pass(entt::entity pass) {
		return registry.get<RenderGraphPass>(pass);
	}
	RenderGraphPass& get_epilogue_pass() {
		return get_pass(epiloguePass);
	}	
	void execute(RHI::CommandList* cmd);	
};