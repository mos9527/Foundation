#include "RenderGraph.hpp" 
#include "RenderGraphResourceCache.hpp"

using namespace RHI;
template<rg_defined_resource T> T* RenderGraph::get_created(rg_handle handle) {
	return &cache.get_cached<T>(handle);
}

void RenderGraph::execute(RenderGraphResourceCache& cache, RHI::CommandList* cmd) {
	rg_context context{ .graph = this, .cmd = cmd };
	Device* device = cmd->GetParent();
	cache.update(*this, device);
	build_graph();
	for (auto& layer : rg_layers) {
		// setup resource barriers
		rg_resources reads, writes, readwrites;
		for (auto entity : layer) {
			RenderPass& pass = registry.get<RenderPass>(entity);
			reads.insert(pass.reads.begin(), pass.reads.end());
			writes.insert(pass.writes.begin(), pass.writes.end());
			readwrites.insert(pass.readwrites.begin(), pass.readwrites.end());
		}
		for (auto& read : reads) {
			if (readwrites.contains(read)) continue;
			auto res = get_as_resource(read);
			auto const& desc = res->GetDesc();

			ResourceState state = ResourceState::Common;
			if (desc.allowShaderResourceView()) state |= ResourceState::PixelShaderResource;
			if (desc.allowUnorderedAccess()) state |= ResourceState::UnorderedAccess;
			res->SetBarrier(cmd, state);
		}

		for (auto& write : writes) {
			if (readwrites.contains(write)) continue;
			auto res = get_as_resource(write);
			auto const& desc = res->GetDesc();

			ResourceState state = ResourceState::Common;
			if (desc.allowShaderResourceView()) state |= ResourceState::PixelShaderResource;
			if (desc.allowUnorderedAccess()) state |= ResourceState::UnorderedAccess;
			if (desc.allowRenderTarget()) state |= ResourceState::RenderTarget;
			if (desc.allowDepthStencil()) state |= ResourceState::DepthWrite;
			res->SetBarrier(cmd, state);
		}

		for (auto& rw : readwrites) {
			auto res = get_as_resource(rw);
			CHECK(res->GetDesc().allowUnorderedAccess());
			res->SetBarrier(cmd, ResourceState::UnorderedAccess);
		}
		// all resources barriers & states are ready at this point
		for (auto entity : layer) {
			RenderPass& pass = registry.get<RenderPass>(entity);
			// invoke!
			if (pass.has_execute())
				(*pass.executes)();
		}		
	}
}