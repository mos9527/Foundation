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

typedef std::function<void(RgContext&)> RgFunction;
typedef unordered_set<RgHandle, DefaultAllocator<RgHandle>> RgResources;
typedef vector<entt::entity, DefaultAllocator<entt::entity>> RgRndPasses;

class RenderGraph;
struct RenderPass {
	friend class RenderGraph;
private:
	entt::entity rg_entity;
public:
	const wchar_t* name = nullptr;
	// r/w/rws for Created resources
	RgResources reads, writes, readwrites;	
	// function callback for actual rendering work
	RgFunction executes;

	RenderPass(entt::entity entity, const wchar_t* name) : rg_entity(entity), name(name) {};
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
    RenderPass& execute(RgFunction&& func) {
		executes = std::move(func);
		return *this;
	}
	bool has_execute() { return executes != nullptr; }
	bool has_dependencies() { return reads.size() > 0 || readwrites.size() > 0; }
	bool reads_from(RgHandle resource) { return reads.contains(resource) || readwrites.contains(resource); }
	bool writes_to(RgHandle resource) { return writes.contains(resource); }
};

// DAG Graph for managing rendering work
// * RenderGraph is meant to be created / destroyed every frame for execution. Which by itself is pretty cheap.
// * To releive resource creation costs, RenderGraphResourceCache caches them so it can be reused when applicable
class RenderGraph : DAG<entt::entity> {	
	friend class RenderGraphResourceCache;
	template<typename T> using Allocator = DefaultAllocator<T>;
	
	RenderGraphResourceCache& cache;	
	entt::basic_registry<entt::entity, Allocator <entt::entity>> registry;		
	entt::entity epiloguePass;
	
	RgRndPasses passes;
	vector<RgRndPasses, Allocator <RgRndPasses>> layers;	

	void build_graph() {
		layers.clear();
		for (entt::entity current : passes) {
			auto& pass = registry.get<RenderPass>(current);
			// create adjacency list from read-writes
			if (pass.has_dependencies()) {
				for (auto& other : passes) {
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
		layers.resize(max_depth + 1);
		for (auto& [node, depth] : depths) layers[depth].push_back(node);
	}
public:	
	RenderGraph(RenderGraphResourceCache& cache) : cache(cache) {
		epiloguePass = registry.create();
		registry.emplace<RenderPass>(epiloguePass, epiloguePass, L"Epilogue");
		passes.push_back(epiloguePass);
	};
	RenderPass& add_pass(const wchar_t* name=L"<unamed>") {
		entt::entity rg_entity = registry.create();
		passes.push_back(rg_entity);
		return registry.emplace<RenderPass>(rg_entity,rg_entity,name);		
	}
	// created resources -> stored as handles
	// note: resource object itself is cached. see RenderGraphResourceCache	
	template<RgDefinedResource T> RgHandle create(RgResourceTraits<T>::desc_type const& desc){
		using traits = RgResourceTraits<T>;
		auto entity = registry.create();
		auto resource = traits::type();		
		resource.desc = desc;
		resource.entity = entity;
		registry.emplace<typename traits::type>(entity, resource);
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
	template<RgDefinedResource T> T* get_created(RgHandle handle) {
		return &cache.get_cached<T>(handle);
	}
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
	RenderPass& get_epilogue_pass() {
		return get_pass(epiloguePass);
	}	
	void execute(RHI::CommandList* cmd);	
};