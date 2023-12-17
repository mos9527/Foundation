#pragma once
#include "RenderGraphResource.hpp"
#include "../../Common/Allocator.hpp"
#include "../RHI/RHI.hpp"

class RenderGraph;
// RHI resource cache for Created resources in RenderGraph
// Basically, a stripped down (and much dumbified) version of entt::registry
// * xxx use entt::resource_cache
class RenderGraphResourceCache {	
	template<typename T> using Allocator = DefaultAllocator<T>;		
	std::unordered_map<ref_type_info, entt::basic_any<>> storages;	
	template<RgDefinedResource T> using storage_type = std::unordered_map<entt::entity, std::shared_ptr<T>>;
	// storages only cares about the entity ID. their versions does not matter.
public:
	void update(RenderGraph& graph, RHI::Device* device);
	void clear() { storages.clear(); }
	template<RgDefinedResource T> storage_type<T>& storage() {
		auto& any_ptr = storages[typeid(storage_type<T>)];
		if (!any_ptr) any_ptr.emplace<storage_type<T>>();
		auto* final_ptr = reinterpret_cast<storage_type<T>*>(any_ptr.data());		
		return *final_ptr;
	}
	template<RgDefinedResource T> bool contains(entt::entity handle) {
		auto& storage_ = storage<T>();
		return storage_.contains(handle);
	}
	template<RgDefinedResource T, typename... Args> auto& emplace_or_replace(entt::entity handle, Args &&... args) {
		auto& storage_ = storage<T>();		
		storage_[handle] = std::make_shared<T>(std::forward<decltype(args)>(args)...);
		return *storage_[handle];
	}
	template<RgDefinedResource T> void erase(entt::entity handle) {
		storage<T>().erase(handle);
	}
	template<RgDefinedResource T> T* get(entt::entity handle) {
		T* ptr = storage<T>()[handle].get();
		if (!ptr) {
			LOG(ERROR) << "Cannot find handle " << entt::to_integral(handle);
		}
		CHECK(ptr && "Resource does not exist. Did you read from the cache before RenderGraph's execute()? Or perhaps you specified the wrong type...");
		return ptr;
	}
	RenderGraphResourceCache() {};
	RenderGraphResourceCache(const RenderGraphResourceCache&) = delete;
	RenderGraphResourceCache(RenderGraphResourceCache&&) = default;
};