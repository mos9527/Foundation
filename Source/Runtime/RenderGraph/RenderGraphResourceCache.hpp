#pragma once
#include "../RHI/RHI.hpp"
#include "RenderGraphResource.hpp"
#include "../../Common/Allocator.hpp"
class RenderGraph;
// RHI resource cache for Created resources in RenderGraph 
class RenderGraphResourceCache {	
	template<typename T> using Allocator = DefaultAllocator<T>;	
	unordered_map<RgHandle, RHI::Buffer, Allocator<std::pair<const RgHandle, RHI::Buffer>>> bufferCache;
	unordered_map<RgHandle, RHI::Texture, Allocator<std::pair<const RgHandle, RHI::Texture>>> textureCache;
	unordered_map<RgHandle, RHI::ShaderResourceView, Allocator<std::pair<const RgHandle, RHI::ShaderResourceView>>> srvCache;
	unordered_map<RgHandle, RHI::RenderTargetView, Allocator<std::pair<const RgHandle, RHI::RenderTargetView>>> rtvCache;
	unordered_map<RgHandle, RHI::DepthStencilView, Allocator<std::pair<const RgHandle, RHI::DepthStencilView>>> dsvCache;
	unordered_map<RgHandle, RHI::UnorderedAccessView, Allocator<std::pair<const RgHandle, RHI::UnorderedAccessView>>> uavCache;
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
	
	template<typename T> auto& get_cache_storage() {}
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
	template<typename T, typename... Args> auto emplace_or_replace(RgHandle handle, Args &&... args) {
		auto& storage = get_cache_storage<T>();
		if (cache_contains<T>(handle)) storage.erase(handle); // emplace, try_emplace does nothing if the key already exisits
		return storage.try_emplace(handle, std::forward<decltype(args)>(args)...);
	}
	template<typename T> auto& get_cached(RgHandle handle) {		
		return get_cache_storage<T>().at(handle);
	}
};