#include "RenderGraph.hpp" 
#include "RenderGraphResourceCache.hpp"

using namespace RHI;
void RenderGraph::execute(RHI::CommandList* cmd) {
	RgContext context{ .graph = this, .cmd = cmd };
	Device* device = cmd->GetParent();
	cache.update(*this, device);
	build_graph();
	for (auto& layer : rg_layers) {
		// setup resource barriers
		RgResources reads, writes, readwrites;
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
			if (desc.allowUnorderedAccess()) 
				state |= ResourceState::UnorderedAccess;
			if (state != ResourceState::Common)
				res->SetBarrier(cmd, state);
		}

		for (auto& write : writes) {
			if (readwrites.contains(write)) continue;
			auto res = get_as_resource(write);
			auto const& desc = res->GetDesc();

			ResourceState state = ResourceState::Common;
			if (desc.allowUnorderedAccess())
				state |= ResourceState::UnorderedAccess;
			if (desc.allowRenderTarget()) 
				state |= ResourceState::RenderTarget;
			if (desc.allowDepthStencil()) 
				state |= ResourceState::DepthWrite;
			if (state != ResourceState::Common) 
				res->SetBarrier(cmd, state);
		}

		for (auto& rw : readwrites) {
			auto res = get_as_resource(rw);
			CHECK(res->GetDesc().allowUnorderedAccess());
			res->SetBarrier(cmd, ResourceState::UnorderedAccess);
		}
		// all resources barriers & states are ready at this point
		RgContext ctx{
			.graph = this,
			.cache = &cache,
			.cmd = cmd
		};		
		PIXBeginEvent(cmd->GetNativeCommandList(),0, L"RenderGraph Execution");
		for (auto entity : layer) {
			RenderPass& pass = registry.get<RenderPass>(entity);
			// invoke!
			if (pass.has_execute())
				(pass.executes)(ctx);
		}		
		PIXEndEvent(cmd->GetNativeCommandList());
	}
}