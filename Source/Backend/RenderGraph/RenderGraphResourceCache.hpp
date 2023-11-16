#pragma once
#include "../RHI/RHI.hpp"
#include "RenderGraphResource.hpp"
class RenderGraph;
// RHI resource cache for Created resources in RenderGraph
class RenderGraphResourceCache {	
	std::unordered_map<RgHandle, RHI::Buffer> bufferCache;
	std::unordered_map<RgHandle, RHI::Texture> textureCache;
	std::unordered_map<RgHandle, RHI::ShaderResourceView> srvCache;
	std::unordered_map<RgHandle, RHI::RenderTargetView> rtvCache;
	std::unordered_map<RgHandle, RHI::DepthStencilView> dsvCache;
	std::unordered_map<RgHandle, RHI::UnorderedAccessView> uavCache;
public:
	void clear() {
		bufferCache.clear();
		textureCache.clear();
		srvCache.clear();
		rtvCache.clear();
		dsvCache.clear();
		uavCache.clear();
	}
	void update(RenderGraph& graph, RHI::Device* device);
	
	template<typename T> auto& get_cache_storage() { 
		static_assert(false || "Not implemented");
		return bufferCache;
	}
	template<> auto& get_cache_storage<RHI::Buffer>() { return bufferCache; }
	template<> auto& get_cache_storage<RHI::Texture>() { return textureCache; }
	template<> auto& get_cache_storage<RHI::ShaderResourceView>() { return srvCache; }
	template<> auto& get_cache_storage<RHI::RenderTargetView>() { return rtvCache; }
	template<> auto& get_cache_storage<RHI::DepthStencilView>() { return dsvCache; }
	template<> auto& get_cache_storage<RHI::UnorderedAccessView>() { return uavCache; }
	template<typename T> bool cache_contains(RgHandle handle) {
		auto& storage = get_cache_storage<T>();
		return storage.find(handle) != storage.end();
	}
	template<typename T> void emplace_or_replace(RgHandle handle, auto&&... args) {
		auto& storage = get_cache_storage<T>();
		if (cache_contains<T>(handle)) storage.erase(handle); // emplace, try_emplace does nothing if the key already exisits
		storage.try_emplace(handle, std::forward<decltype(args)>(args)...);
	}
	template<typename T> auto& get_cached(RgHandle handle) {		
		return get_cache_storage<T>().at(handle);
	}
};