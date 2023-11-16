#include "RenderGraph.hpp"
#include "RenderGraphResourceCache.hpp"

void RenderGraphResourceCache::update(RenderGraph& graph, RHI::Device* device) {
	auto& rg_registry = graph.registry;
	std::unordered_map<RgHandle, bool> resource_dirty;
	// imported resources are left as is since we'd expect them to be usable at all times within the registry	
	// created resources may mutate over different graph iterations (i.e. changed buffer sizes) so we'd watch them here:
	for (auto& buffer : rg_registry.storage<RgBuffer>()) {
		auto& handle = rg_registry.get<RgHandle>(buffer.entity);		
		if (!cache_contains<RHI::Buffer>(handle) || get_cached<RHI::Buffer>(handle).GetDesc() != buffer.desc)
		{
			// rebuild : resource is dirty
			emplace_or_replace<RHI::Buffer>(handle, device, buffer.desc);
			resource_dirty[handle] = true;
			DLOG(INFO) << "Rebuilt buffer " << entt::to_integral(handle.entity);
		}
	}
	for (auto& texture : rg_registry.storage<RgTexture>()) {
		auto& handle = rg_registry.get<RgHandle>(texture.entity);
		if (!cache_contains<RHI::Texture>(handle) || get_cached<RHI::Texture>(handle).GetDesc() != texture.desc) {
			textureCache.erase(handle);
			emplace_or_replace<RHI::Texture>(handle, device, texture.desc);
			resource_dirty[handle] = true;
			DLOG(INFO) << "Rebuilt texture " << entt::to_integral(handle.entity);
		}
	}
	// resource views
	auto build_view = [&]<typename Cache>(auto & view, Cache& cache) -> void {
		auto& viewing_handle = rg_registry.get<RgHandle>(view.entity);
		auto& viewed_handle = rg_registry.get<RgHandle>(view.desc.viewed);
		RHI::Resource* ptr = nullptr;
		switch (viewed_handle.type) {
		case RgResourceType::Buffer:
			ptr = viewed_handle.is_imported() ? graph.get_imported<RHI::Buffer>(viewed_handle) : &get_cached<RHI::Buffer>(viewed_handle);
		case RgResourceType::Texture:
			ptr = viewed_handle.is_imported() ? graph.get_imported<RHI::Texture>(viewed_handle) : &get_cached<RHI::Texture>(viewed_handle);
		}
		using view_type = Cache::mapped_type;		
		if (
			!cache_contains<view_type>(viewing_handle) || 
			/*cache[viewing_handle].GetDesc() != view.desc.viewDesc ||*/ // xxx resource view comp
			resource_dirty.find(viewed_handle) != resource_dirty.end()
		) {
			// rebuild : view (or resource) is dirty			
			emplace_or_replace<view_type>(viewing_handle, ptr, view.desc.viewDesc);
			DLOG(INFO) << "Rebuilt view " << entt::to_integral(viewing_handle.entity);
		}
	};

	for (auto& srv : rg_registry.storage<RgSRV>()) build_view(srv, srvCache);
	for (auto& rtv : rg_registry.storage<RgRTV>()) build_view(rtv, rtvCache);
	for (auto& dsv : rg_registry.storage<RgDSV>()) build_view(dsv, dsvCache);
	for (auto& uav : rg_registry.storage<RgUAV>()) build_view(uav, uavCache);
}