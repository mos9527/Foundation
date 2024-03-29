#include "RenderGraph.hpp" 
#include "RenderGraphResourceCache.hpp"

using namespace RHI;
void RenderGraph::build_graph() {	
	ZoneScopedN("RenderGraph Building");
	layers.clear();
	for (auto current : passes) {
		auto& pass = registry.get<RenderGraphPass>(entt::entity(current));
		// create adjacency list from read-writes
		if (pass.has_dependencies()) {
			for (auto& other : passes) {
				if (other != current) {
					auto& other_pass = registry.get<RenderGraphPass>(entt::entity(other));
					for (auto& write : other_pass.writes) {
						if (pass.reads_from(write)) {
							add_edge(other, current);
						}
					}
					for (auto& readwrite : other_pass.readwrites) {
						if (pass.reads_from(readwrite)) {
							add_edge(other, current);
						}
					}
				}
			}
		}
	}
	// seperate passes into layers		
	auto& topsorted = topological_sort();
	auto& depths = get_depths(topsorted);
	ENTT_ID_TYPE max_depth = 0;
	for (auto depth : depths) max_depth = std::max(max_depth, depth);
	layers.resize(max_depth + 1);
	for (auto v : topsorted) layers[depths[v]].push_back(v);
}
void RenderGraph::execute(RHI::CommandList* cmd) {
	ZoneScopedN("RenderGraph Execution");
	RgContext ctx{ .graph = this,.cache = &cache,.cmd = cmd };
	Device* device = cmd->GetParent();
	cache.update(*this, device);
	build_graph();
	cmd->BeginEvent(L"RenderGraph Execution");
	for (auto& layer : layers) {
		// setup resource barriers
		RgResources reads, writes, readwrites;
		for (auto entity : layer) {
			RenderGraphPass& pass = registry.get<RenderGraphPass>(entt::entity(entity));
			/* reads */
			reads.insert(pass.reads.begin(), pass.reads.end());
			/* writes */
			writes.insert(pass.writes.begin(), pass.writes.end());
			/* readwrites */
			readwrites.insert(pass.readwrites.begin(), pass.readwrites.end());
		}
		for (auto& read : reads) {
			if (readwrites.contains(read)) continue;			
			auto res = get_as_resource(read);
			if (!res) continue;

			if (cmd->GetType() == CommandListType::Direct) {
				auto const& desc = res->GetDesc();
				ResourceState state = ResourceState::PixelShaderResource;
				if (desc.allowUnorderedAccess()) {
					state |= ResourceState::NonPixelShaderResoruce;
					cmd->QueueUAVBarrier(res);
				}
				if (state != ResourceState::Common)
					cmd->QueueTransitionBarrier(res, state);
			}
			else if (cmd->GetType() == CommandListType::Compute) {
				if (res->GetDesc().allowUnorderedAccess())
					cmd->QueueUAVBarrier(res);
			}
			else if (cmd->GetType() == CommandListType::Copy) {
				CHECK(false && "Not implemented");
			}

		}		
		for (auto& rw : readwrites) {
			auto res = get_as_resource(rw);
			if (!res) continue;
			CHECK(cmd->GetType() == CommandListType::Direct || cmd->GetType() == CommandListType::Compute && "Invalid type of command list to perform UAV barrier.");
			CHECK(res->GetDesc().allowUnorderedAccess());
			cmd->QueueUAVBarrier(res);
		}		
		for (auto& write : writes) {
			if (readwrites.contains(write)) continue;
			auto res = get_as_resource(write);
			if (!res) continue;
			CHECK(cmd->GetType() == CommandListType::Direct && "Invalid type of command list to perform Write transitions.");

			auto const& desc = res->GetDesc();
			ResourceState state = ResourceState::Common;
			if (desc.allowRenderTarget()) 
				state |= ResourceState::RenderTarget;
			if (desc.allowDepthStencil()) 
				state |= ResourceState::DepthWrite;
			if (state != ResourceState::Common) 
				cmd->QueueTransitionBarrier(res, state);
		}
		cmd->FlushBarriers();
		// all resources barriers & states are ready at this point		
		cmd->BeginEvent(L"RenderGraph Layer");
		for (auto entity : layer) {
			RenderGraphPass& pass = registry.get<RenderGraphPass>(entt::entity(entity));
			cmd->BeginEvent(pass.name);
			// invoke!
			if (pass.has_execute()) {
				(pass.executes)(ctx);
			}
			cmd->EndEvent();
		}
		cmd->EndEvent();
	}
	cmd->EndEvent();
}