#include "AreaReduce.hpp"
using namespace RHI;
AreaReducePass::AreaReducePass(Device* device) {
	CS = BuildShader(L"AreaReduce", L"main_reduce_instance", L"cs_6_6");
	RS = std::make_unique<RootSignature>(
		device,
		RootSignatureDesc()
		.SetDirectlyIndexed()
		.AddConstant(0, 0, 6) // Rect X0,Y0,Width,Height,In Texture heap index, Out Buffer UAV heap index
	);
	RS->SetName(L"Area Reduce");
	D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};
	computePsoDesc.pRootSignature = *RS;
	ComPtr<ID3D12PipelineState> pso;
	computePsoDesc.CS = CD3DX12_SHADER_BYTECODE(CS->GetData(), CS->GetSize());
	CHECK_HR(device->GetNativeDevice()->CreateComputePipelineState(&computePsoDesc, IID_PPV_ARGS(&pso)));
	PSO = std::make_unique<PipelineState>(device, std::move(pso));
}

RenderGraphPass& AreaReducePass::insert_reduce_material_instance(RenderGraph& rg, AreaReducePassHandles&& handles, uint2 point, uint2 extent) {
	return rg.add_pass(L"Area Reduce Unique Instances")
		.read(handles.texture)
		.readwrite(handles.output)
		.execute([=](RgContext& ctx) {
			auto* r_texture = ctx.graph->get<Texture>(handles.texture);
			auto* r_in_srv = ctx.graph->get<ShaderResourceView>(handles.textureSRV);
			auto* r_out_uav = ctx.graph->get<UnorderedAccessView>(handles.outputUAV);
			uint width = r_texture->GetDesc().width, height = r_texture->GetDesc().height;
			auto native = ctx.cmd->GetNativeCommandList();
			native->SetPipelineState(*PSO);
			native->SetComputeRootSignature(*RS);
			native->SetComputeRoot32BitConstants(0, 2, &point, 0);
			native->SetComputeRoot32BitConstants(0, 2, &extent, 2);
			native->SetComputeRoot32BitConstant(0, r_in_srv->descriptor.get_heap_handle(), 4);
			native->SetComputeRoot32BitConstant(0, r_out_uav->descriptor.get_heap_handle(), 5);
			native->Dispatch(DivRoundUp(extent.x, RENDERER_FULLSCREEN_THREADS), DivRoundUp(extent.y, RENDERER_FULLSCREEN_THREADS), 1);
		});
}