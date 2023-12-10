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
	inline RenderGraphPass& write(RgHandle& resource) {
		CHECK(check_handle(resource));
		resource.version++;
		writes.insert(resource); 
		return *this; 
	}
	inline RenderGraphPass& readwrite(RgHandle& resource) {
		CHECK(check_handle(resource));
		resource.version++;
		readwrites.insert(resource);
		return *this;
	}
	inline RenderGraphPass& read(RgHandle& resource) {
		CHECK(check_handle(resource));
		reads.insert(resource);
		return *this;
	}
	inline RenderGraphPass& execute(RgFunction&& func) {
		executes = std::move(func);
		return *this;
	}
	inline bool has_execute() { return executes != nullptr; }
	inline bool has_dependencies() { return reads.size() > 0 || readwrites.size() > 0; }
	inline bool reads_from(RgHandle resource) { return reads.contains(resource) || readwrites.contains(resource); }
	inline bool writes_to(RgHandle resource) { return writes.contains(resource) || readwrites.contains(resource); }
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

	void build_graph();
public:	
	RenderGraph(RenderGraphResourceCache& cache) : cache(cache) {
		epiloguePass = registry.create();
		registry.emplace<RenderGraphPass>(epiloguePass, epiloguePass, L"Epilogue");
		passes.push_back(epiloguePass);
	};
	inline RenderGraphPass& add_pass(const wchar_t* name=L"<unamed>") {
		entt::entity rg_entity = registry.create();
		passes.push_back(rg_entity);
		return registry.emplace<RenderGraphPass>(rg_entity,rg_entity,name);		
	}
	// created/transient resources -> stored as handles	
	// note: resource object itself is cached. see RenderGraphResourceCache	
	template<RgDefinedResource T> inline RgHandle& create(RgResourceTraits<T>::desc_type const& desc){
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
	// imported resources -> stored as pointers
	template<RgDefinedResource T> inline RgHandle& import(RgResourceTraits<T>::rhi_type* imported) {
		using traits = RgResourceTraits<T>;
		auto entity = registry.create();
		registry.emplace<typename RgResourceTraits<T>::rhi_type*>(entity, imported);
		RgHandle handle{
			.version = 0,
			.type = traits::type_enum,
			.entity = entity,
			.imported = true
		};
		return registry.emplace<RgHandle>(entity, handle);;
	}
	// retrives imported / created RHI object pointer
	template<RgDefinedResource T> inline T* get(RgHandle const& handle) {
		if (!handle.imported) // created objects are stored in the cache
			return cache.get<T>(handle);
		else // pointers are stored as data in RG registry
			return registry.get<T*>(handle);
	}
	// retrives in-graph object reference
	// RenderGraph contains RgHandle, and RgResource dervied objects
	template<typename T> inline  T& get(RgHandle const& handle) {
		return registry.get<T>(handle);
	}
	// retrives imported / created RHI object as Resource*
	// if not convertible, nullptr is returned
	// * applicalbe to Buffer & Texture
	inline RHI::Resource* get_as_resource(RgHandle const& handle) {
		if (handle.type == RgResourceType::Buffer) return static_cast<RHI::Resource*>(get<RHI::Buffer>(handle));
		if (handle.type == RgResourceType::Texture) return static_cast<RHI::Resource*>(get<RHI::Texture>(handle));
		return nullptr;
	}
	inline RenderGraphPass& get_pass(entt::entity pass) {
		return registry.get<RenderGraphPass>(pass);
	}
	inline RenderGraphPass& get_epilogue_pass() {
		return get_pass(epiloguePass);
	}	
	void execute(RHI::CommandList* cmd);	
};