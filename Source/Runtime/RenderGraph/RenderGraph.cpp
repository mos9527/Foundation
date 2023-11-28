#include "RenderGraph.hpp" 
#include "RenderGraphResourceCache.hpp"

using namespace RHI;
void RenderGraph::build_graph() {	
	layers.clear();
	for (entt::entity current : passes) {
		auto& pass = registry.get<RenderGraphPass>(current);
		// create adjacency list from read-writes
		if (pass.has_dependencies()) {
			for (auto& other : passes) {
				if (other != current) {
					auto& other_pass = registry.get<RenderGraphPass>(other);
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
	auto topsorted = topological_sort();
	if (!topsorted.size() && graph.size()) {
		// cyclic dependency
		LOG(ERROR) << "Cyclic dependency in RenderGraph. Consider using import().";
		LOG(INFO) << "Current Dependency Graph";
		set_type visited;
		auto dfs_nodes = [&](auto& func, vertex_type current, uint depth) -> void {
			auto& pass = registry.get<RenderGraphPass>(current);
			std::string prefix; for (uint i = 0; i < depth; i++) prefix += '\t';
			LOG(INFO) << prefix << wstring_to_utf8(pass.name);
			if (visited.contains(current)) {
				LOG(ERROR) << prefix << " ^ Potential loop here.";
				return;
			}
			visited.insert(current);
			for (auto child : graph[current])
				func(func, child, depth + 1);
		};
		for (auto& [vtx, tree] : graph)
			dfs_nodes(dfs_nodes, vtx, 0);
#ifdef _DEBUG
		__debugbreak();
#endif
	}
	auto depths = get_depths(topsorted);
	uint max_depth = 0;
	for (auto& [node, depth] : depths) max_depth = std::max(max_depth, depth);
	layers.resize(max_depth + 1);
	for (auto& [node, depth] : depths) layers[depth].push_back(node);
}
void RenderGraph::execute(RHI::CommandList* cmd) {
	RgContext ctx{ .graph = this,.cache = &cache,.cmd = cmd };
	Device* device = cmd->GetParent();
	cache.update(*this, device);
	build_graph();
	PIXBeginEvent(cmd->GetNativeCommandList(), 0, L"RenderGraph Execution");
	for (auto& layer : layers) {
		// setup resource barriers
		RgResources reads, writes, readwrites;
		for (auto entity : layer) {
			RenderGraphPass& pass = registry.get<RenderGraphPass>(entity);
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
				if (desc.allowUnorderedAccess()) 
					state |= ResourceState::NonPixelShaderResoruce;
				if (state != ResourceState::Common)
					cmd->QueueTransitionBarrier(res, state);
			}
			else if (cmd->GetType() == CommandListType::Compute) {
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