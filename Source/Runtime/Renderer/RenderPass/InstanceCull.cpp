#include "InstanceCull.hpp"
using namespace RHI;
void InstanceCull::reset() {
	build_shader(CS_Early, 0, L"main_early", L"cs_6_6");
	build_shader(CS_Late, 0, L"main_late", L"cs_6_6");
	D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
	desc.pRootSignature = *EditorGlobals::g_RHI.rootSig;
	desc.CS = CD3DX12_SHADER_BYTECODE(CS_Early->GetData(), CS_Early->GetSize());
	ComPtr<ID3D12PipelineState> pso;
	CHECK_HR(device->GetNativeDevice()->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso)));
	PSO_Early = std::make_unique<PipelineState>(device, std::move(pso));	
	PSO_Early->SetName(L"Early Cull");
	desc.CS = CD3DX12_SHADER_BYTECODE(CS_Late->GetData(), CS_Late->GetSize());
	CHECK_HR(device->GetNativeDevice()->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso)));
	PSO_Late = std::make_unique<PipelineState>(device, std::move(pso));	
	PSO_Late->SetName(L"Late Cull");

	visibility.first = std::make_unique<Buffer>(device, Resource::ResourceDesc::GetGenericBufferDesc(
		MAX_INSTANCE_COUNT, RAW_BUFFER_STRIDE, ResourceState::CopyDest, ResourceHeapType::Default,
		ResourceFlags::UnorderedAccess, L"Instance Visibilties"
	));
	visibility.second = std::make_unique<UnorderedAccessView>(visibility.first.get(), UnorderedAccessViewDesc::GetRawBufferDesc(0, visibility.first->GetDesc().numElements(), 0));
	constants = std::make_unique<BufferContainer<InstanceCullConstant>>(device, 1, L"Indirect Constants");
	constants->Data()->visibilityIndex = visibility.second->get_persistent_descriptor().get_heap_handle();
}

RenderGraphPass& InstanceCull::insert(RenderGraph& rg, SceneView* sceneView, Handles const& handles, bool late) {
	const auto setup_pass = [=](RenderGraphPass& pass) {
		for (int i = 0; i < INSTANCE_CULL_MAX_CMDS; i++) {
			auto const& cmd = handles.cmd_uav_instanceMaskAllow_instanceMaskRejcectClearCounter[i];
			auto p_cmd = std::get<0>(cmd);
			if (p_cmd) pass.readwrite(*p_cmd);
			constants->Data()->cmds[i].instanceAllowMask = std::get<2>(cmd);
			constants->Data()->cmds[i].instanceRejectMask = std::get<3>(cmd);
		}
	};
	if (!late) {
		// Early cull: Checks all previously visible instances & preforms Frustum Culling on them
		auto& pass = rg.add_pass(L"Instance Cull Early Pass");
		setup_pass(pass);		
		pass.execute([=](RgContext& ctx) {
			auto native = ctx.cmd->GetNativeCommandList();
			for (int i = 0;i < INSTANCE_CULL_MAX_CMDS; i++){				
				auto p_uav = std::get<1>(handles.cmd_uav_instanceMaskAllow_instanceMaskRejcectClearCounter[i]);
				constants->Data()->cmds[i].cmdIndex = p_uav ? ctx.graph->get<UnorderedAccessView>(*p_uav)->allocate_transient_descriptor(ctx.cmd).get_heap_handle() : INVALID_HEAP_HANDLE;
				auto p_cmd = std::get<0>(handles.cmd_uav_instanceMaskAllow_instanceMaskRejcectClearCounter[i]);
				if (p_cmd && std::get<4>(handles.cmd_uav_instanceMaskAllow_instanceMaskRejcectClearCounter[i])) {
					// Clear counters
					auto r_cmd = ctx.graph->get<Buffer>(*p_cmd);
					ctx.cmd->QueueTransitionBarrier(r_cmd, ResourceState::CopyDest);					
					ctx.cmd->FlushBarriers();
					ctx.cmd->ZeroBufferRegion(r_cmd, CommandBufferCounterOffset, sizeof(UINT));
					ctx.cmd->QueueTransitionBarrier(r_cmd, ResourceState::UnorderedAccess);
					ctx.cmd->FlushBarriers();
				}
			}
			native->SetPipelineState(*PSO_Early);
			native->SetComputeRootSignature(*EditorGlobals::g_RHI.rootSig);
			native->SetComputeRootConstantBufferView(RHIContext::ROOTSIG_CB_EDITOR_GLOBAL, sceneView->GetGlobalsBuffer().GetGPUAddress());
			native->SetComputeRootConstantBufferView(RHIContext::ROOTSIG_CB_SHADER_GLOBAL, constants->GetGPUAddress());
			native->Dispatch(DivRoundUp(sceneView->GetGlobalsBuffer().Data()->meshInstances.count, RENDERER_INSTANCE_CULL_THREADS), 1, 1);
		});
		return pass;
	}
	else {
		// Late cull : Check for false-negatives with Occlusion Culling & Frustum Culling
		CHECK(handles.hiz_srv.first && handles.hiz_srv.second && "Need depth pyramid");
		auto& pass = rg.add_pass(L"Instance Cull Late Pass");
		setup_pass(pass);
		pass.read(*handles.hiz_srv.first);
		pass.execute([=](RgContext& ctx) {
			auto native = ctx.cmd->GetNativeCommandList();
			for (int i = 0; i < INSTANCE_CULL_MAX_CMDS; i++) {
				auto p_uav = std::get<1>(handles.cmd_uav_instanceMaskAllow_instanceMaskRejcectClearCounter[i]);
				constants->Data()->cmds[i].cmdIndex = p_uav ? ctx.graph->get<UnorderedAccessView>(*p_uav)->allocate_transient_descriptor(ctx.cmd).get_heap_handle() : INVALID_HEAP_HANDLE;
				auto p_cmd = std::get<0>(handles.cmd_uav_instanceMaskAllow_instanceMaskRejcectClearCounter[i]);
				if (p_cmd && std::get<4>(handles.cmd_uav_instanceMaskAllow_instanceMaskRejcectClearCounter[i])) {
					// Clear counters
					auto r_cmd = ctx.graph->get<Buffer>(*p_cmd);
					ctx.cmd->QueueTransitionBarrier(r_cmd, ResourceState::CopyDest);
					ctx.cmd->FlushBarriers();
					ctx.cmd->ZeroBufferRegion(r_cmd, CommandBufferCounterOffset, sizeof(UINT));
					ctx.cmd->QueueTransitionBarrier(r_cmd, ResourceState::UnorderedAccess);
					ctx.cmd->FlushBarriers();
				}
			}
			auto* r_hiz = ctx.graph->get<Texture>(*handles.hiz_srv.first);
			auto* r_hiz_srv = ctx.graph->get<ShaderResourceView>(*handles.hiz_srv.second);
			constants->Data()->hizIndex = r_hiz_srv->allocate_transient_descriptor(ctx.cmd).get_heap_handle();
			constants->Data()->hizMips = r_hiz->GetDesc().mipLevels;

			native->SetPipelineState(*PSO_Late);
			native->SetComputeRootSignature(*EditorGlobals::g_RHI.rootSig);
			native->SetComputeRootConstantBufferView(RHIContext::ROOTSIG_CB_EDITOR_GLOBAL, sceneView->GetGlobalsBuffer().GetGPUAddress());
			native->SetComputeRootConstantBufferView(RHIContext::ROOTSIG_CB_SHADER_GLOBAL, constants->GetGPUAddress());
			native->Dispatch(DivRoundUp(sceneView->GetGlobalsBuffer().Data()->meshInstances.count, RENDERER_INSTANCE_CULL_THREADS), 1, 1);
		});
		return pass;
	}
}