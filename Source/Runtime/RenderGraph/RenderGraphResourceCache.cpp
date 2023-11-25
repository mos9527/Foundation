#include "RenderGraph.hpp"

void RenderGraphResourceCache::update(RenderGraph& graph, RHI::Device* device) {
	auto& rg_registry = graph.registry;
	std::unordered_map<entt::entity, bool> resource_dirty;
	// imported resources are left as is since we'd expect them to be usable at all times within the registry	
	// created resources may mutate over different graph iterations (i.e. changed buffer sizes) so we'd watch them here:
	for (auto& buffer : rg_registry.storage<RgBuffer>()) {
		auto handle = buffer.entity;
		if (!contains<RHI::Buffer>(handle) || get<RHI::Buffer>(handle)->GetDesc() != buffer.desc)
		{
			// rebuild : resource is dirty
			emplace_or_replace<RHI::Buffer>(handle, device, buffer.desc);
			resource_dirty[handle] = true;
			auto name = get<RHI::Buffer>(handle)->GetName();
			// DLOG(INFO) << "Rebuilt buffer " << wstring_to_utf8(name ? name : L"<unamed>");
		}
	}
	for (auto& texture : rg_registry.storage<RgTexture>()) {
		auto handle = texture.entity;
		if (!contains<RHI::Texture>(handle) || get<RHI::Texture>(handle)->GetDesc() != texture.desc) {
			device->Wait();			
			emplace_or_replace<RHI::Texture>(handle, device, texture.desc);
			resource_dirty[handle] = true;
			auto name = get<RHI::Texture>(handle)->GetName();
			// DLOG(INFO) << "Rebuilt texture " <<  wstring_to_utf8(name ? name : L"<unamed>");
		}
	}
	// resource views
	auto build_view = [&]<typename Cache>(auto & view, Cache& cache) -> void {
		using view_type = Cache::mapped_type::element_type;

		auto& viewing_handle = rg_registry.get<RgHandle>(view.entity);
		auto& viewed_handle = rg_registry.get<RgHandle>(view.desc.viewed);
		RHI::Resource* ptr = nullptr;
		switch (viewed_handle.type) {
		case RgResourceType::Buffer:
			ptr = get<RHI::Buffer>(viewed_handle);
			break;
		case RgResourceType::Texture:
			ptr = get<RHI::Texture>(viewed_handle);
			break;
		}
		if (
			!contains<view_type>(viewing_handle) || 
			get<view_type>(viewing_handle)->GetDesc() != view.desc.viewDesc ||
			resource_dirty.find(viewed_handle) != resource_dirty.end()
		) {
			// rebuild : view (or resource) is dirty			
			if constexpr(std::is_same_v<RHI::UnorderedAccessView, view_type>){
				// UAVs may have counter resources
				RHI::Resource* counter_ptr = nullptr;
				if (view.desc.viewedCounter != entt::tombstone) {
					auto& viewed_counter_handle = rg_registry.get<RgHandle>(view.desc.viewedCounter);
					switch (viewed_counter_handle.type) {
					case RgResourceType::Buffer:
						counter_ptr = get<RHI::Buffer>(viewed_counter_handle);
						break;
					case RgResourceType::Texture:
						counter_ptr = get<RHI::Texture>(viewed_counter_handle);
						break;
					}
				}
				emplace_or_replace<view_type>(viewing_handle, ptr, counter_ptr, view.desc.viewDesc);
			}
			else {
				emplace_or_replace<view_type>(viewing_handle, ptr, view.desc.viewDesc);
			}
			// DLOG(INFO) << "Rebuilt view #" << entt::to_integral(viewing_handle.entity) << ". Which views " << wstring_to_utf8(ptr->GetName() ? ptr->GetName() : L"<unamed>");
		}
	};

	for (auto& srv : rg_registry.storage<RgSRV>()) build_view(srv, storage<RHI::ShaderResourceView>());
	for (auto& rtv : rg_registry.storage<RgRTV>()) build_view(rtv, storage<RHI::RenderTargetView>());
	for (auto& dsv : rg_registry.storage<RgDSV>()) build_view(dsv, storage<RHI::DepthStencilView>());
	for (auto& uav : rg_registry.storage<RgUAV>()) build_view(uav, storage<RHI::UnorderedAccessView>());
}