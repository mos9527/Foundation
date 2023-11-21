#include "IndirectLODCull.hpp"
using namespace RHI;
IndirectLODCullPass::IndirectLODCullPass(Device* device) {
	cullPassCS = std::make_unique<Shader>(L"Shaders/IndirectLODCull.hlsl", L"main", L"cs_6_6");
	cullPassRS = std::make_unique<RootSignature>(
		device,
		RootSignatureDesc()
		.AddConstantBufferView(0, 0) // b0 space0 : SceneGlobals	
		.AddShaderResourceView(0, 0) // t0 space0 : SceneMeshInstance
		.AddUnorderedAccessViewWithCounter(0, 0)// u0 space0 : Indirect Commandlists
	);
	cullPassRS->SetName(L"Indirect Cull & LOD Classification");
	D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};
	computePsoDesc.pRootSignature = *cullPassRS;
	computePsoDesc.CS = CD3DX12_SHADER_BYTECODE(cullPassCS->GetData(), cullPassCS->GetSize());
	ComPtr<ID3D12PipelineState> pso;
	CHECK_HR(device->GetNativeDevice()->CreateComputePipelineState(&computePsoDesc, IID_PPV_ARGS(&pso)));
	cullPassPSO = std::make_unique<PipelineState>(device, std::move(pso));	
}
void IndirectLODCullPass::insert(RenderGraph& rg, SceneGraphView* sceneView, IndirectLODCullPassHandles& handles) {
	rg.add_pass(L"Indirect Cull & LOD")
		.readwrite(handles.indirectCmdBuffer)		
		.execute([=](RgContext& ctx) -> void {
			auto* r_indirect_cmd_buffer = ctx.graph->get<Buffer>(handles.indirectCmdBuffer);
			auto* r_indirect_cmd_buffer_uav = ctx.graph->get<UnorderedAccessView>(handles.indirectCmdBufferUAV);
			auto native = ctx.cmd->GetNativeCommandList();
			native->SetPipelineState(*cullPassPSO);
			native->SetComputeRootSignature(*cullPassRS);
			native->SetComputeRootConstantBufferView(0, sceneView->get_SceneGlobalsBuffer()->GetGPUAddress());
			native->SetComputeRootShaderResourceView(1, sceneView->get_SceneMeshInstancesBuffer()->GetGPUAddress());
			native->SetComputeRootDescriptorTable(2, r_indirect_cmd_buffer_uav->descriptor);
			r_indirect_cmd_buffer->SetBarrier(ctx.cmd, ResourceState::CopyDest);
			ctx.cmd->ZeroBufferRegion(r_indirect_cmd_buffer, CommandBufferCounterOffset, sizeof(UINT));			
			r_indirect_cmd_buffer->SetBarrier(ctx.cmd, ResourceState::UnorderedAccess);
			// dispatch compute to cull on the gpu
			native->Dispatch(DivRoundUp(sceneView->get_SceneGlobals().numMeshInstances, RENDERER_INSTANCE_CULL_THREADS), 1, 1);
	});
}