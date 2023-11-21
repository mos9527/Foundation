#pragma once
#include "../RHI/RHI.hpp"
#include "RenderGraphResource.hpp"
#include "../../Common/Allocator.hpp"

class RenderGraph;
// RHI resource cache for Created resources in RenderGraph
// Basically, a stripped down (and much dumbified) version of entt::registry
// * We're not using entt::registry since its entity management does not allow random insertion
class RenderGraphResourceCache {	
	template<typename T> using Allocator = DefaultAllocator<T>;		
	unordered_map<ref_type_info, entt::basic_any<>, Allocator<std::pair<const ref_type_info, entt::basic_any<>>>> storages;
	template<RgDefinedResource T> using storage_type = unordered_map<RgHandle, std::shared_ptr<T>, Allocator<std::pair<const RgHandle, std::shared_ptr<T>>>>;

public:
	void update(RenderGraph& graph, RHI::Device* device);
	void clear() { storages.clear(); }
	template<RgDefinedResource T> storage_type<T>& storage() {
		auto& any_ptr = storages[typeid(storage_type<T>)];
		if (!any_ptr) any_ptr.emplace<storage_type<T>>();
		auto* final_ptr = reinterpret_cast<storage_type<T>*>(any_ptr.data());		
		return *final_ptr;
	}
	template<RgDefinedResource T> bool contains(RgHandle handle) {
		auto& storage_ = storage<T>();
		return storage_.contains(handle);
	}
	template<RgDefinedResource T, typename... Args> auto& emplace_or_replace(RgHandle handle, Args &&... args) {
		auto& storage_ = storage<T>();		
		storage_[handle] = std::make_shared<T>(std::forward<decltype(args)>(args)...);
		return *storage_[handle];
	}
	template<RgDefinedResource T> void erase(RgHandle handle) {
		storage<T>().erase(handle);
	}
	template<RgDefinedResource T> T& get(RgHandle handle) {
		return *(storage<T>()[handle].get());
	}
	RenderGraphResourceCache() {};
	RenderGraphResourceCache(const RenderGraphResourceCache&) = delete;
	RenderGraphResourceCache(RenderGraphResourceCache&&) = default;
};