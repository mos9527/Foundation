#include "RenderGraph.hpp" 
#include "RenderGraphResourceCache.hpp"

using namespace RHI;
void RenderGraph::execute(RHI::CommandList* cmd) {
	RgContext ctx{ .graph = this,.cache = &cache,.cmd = cmd };
	Device* device = cmd->GetParent();
	cache.update(*this, device);
	build_graph();
	PIXBeginEvent(cmd->GetNativeCommandList(), 0, L"RenderGraph Execution");
	for (auto& layer : layers) {
		// setup resource barriers
		RgResources reads, writes, readwrites, indirectArgs;
		for (auto entity : layer) {
			RenderGraphPass& pass = registry.get<RenderGraphPass>(entity);
			reads.insert(pass.reads.begin(), pass.reads.end());
			writes.insert(pass.writes.begin(), pass.writes.end());
			readwrites.insert(pass.readwrites.begin(), pass.readwrites.end());
			indirectArgs.insert(pass.indirectArguments.begin(), pass.indirectArguments.end());
		}
		for (auto& read : reads) {
			if (readwrites.contains(read)) continue;
			auto res = get_as_resource(read);
			if (!res) continue;

			auto const& desc = res->GetDesc();
			ResourceState state = ResourceState::PixelShaderResource;
			if (desc.allowUnorderedAccess()) 
				state |= ResourceState::NonPixelShaderResoruce;
			if (state != ResourceState::Common)
				res->SetBarrier(cmd, state);
		}

		for (auto& write : writes) {
			if (readwrites.contains(write)) continue;
			auto res = get_as_resource(write);
			if (!res) continue;

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
			if (!res) continue;

			CHECK(res->GetDesc().allowUnorderedAccess());
			res->SetBarrier(cmd, ResourceState::UnorderedAccess);
		}

		for (auto& indirectArg : indirectArgs) {
			auto res = get_as_resource(indirectArg);
			if (!res) continue;

			res->SetBarrier(cmd, ResourceState::IndirectArgument);
		}
		// all resources barriers & states are ready at this point		
		PIXBeginEvent(cmd->GetNativeCommandList(), 0, L"RenderGraph Layer");
		for (auto entity : layer) {
			RenderGraphPass& pass = registry.get<RenderGraphPass>(entity);
			PIXBeginEvent(cmd->GetNativeCommandList(), 0, pass.name);				
			// invoke!
			if (pass.has_execute()) {
				(pass.executes)(ctx);
			}
			PIXEndEvent(cmd->GetNativeCommandList());
		}		
		PIXEndEvent(cmd->GetNativeCommandList());
	}
	PIXEndEvent(cmd->GetNativeCommandList());
}