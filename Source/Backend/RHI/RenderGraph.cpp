#include "RenderGraph.hpp"
#include <numeric>
template<> rg_resource RenderGraph::create<RHI::Resource>(RHI::Resource::ResourceDesc desc) {
	auto entity = registry.create();
	registry.emplace<rg_created_resource>(entity, desc);
	return rg_resource{
		.version = 0,
		.type = rg_resource::Resource,
		.flag = rg_resource::Created,
		.resource = entity
	};
};

template<> rg_resource RenderGraph::reference<RHI::Resource>(RHI::Resource* resource) {
	auto entity = registry.create();
	registry.emplace<rg_imported_resource>(entity, resource);
	return rg_resource{
		.version = 0,
		.type = rg_resource::Resource,
		.flag = rg_resource::Imported,
		.resource = entity
	};
};

template<> RHI::Resource* RenderGraph::resolve<RHI::Resource>(rg_resource resource) {
	if (resource.flag & rg_resource::Imported) {
		auto ptr = registry.try_get<RHI::Resource*>(resource);
		return ptr ? *ptr : nullptr;
	}
	if (resource.flag & rg_resource::Created)
		return registry.try_get<RHI::Resource>(resource);
};