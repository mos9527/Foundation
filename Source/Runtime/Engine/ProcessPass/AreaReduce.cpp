#include "AreaReduce.hpp"
using namespace RHI;
AreaReducePass::AreaReducePass(Device* device) {
	CS = BuildShader(L"AreaReduce", L"main_reduce_instance", L"cs_6_6");
	D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};
	computePsoDesc.pRootSignature = *EditorGlobals::g_RHI.rootSig;
	ComPtr<ID3D12PipelineState> pso;
	computePsoDesc.CS = CD3DX12_SHADER_BYTECODE(CS->GetData(), CS->GetSize());
	CHECK_HR(device->GetNativeDevice()->CreateComputePipelineState(&computePsoDesc, IID_PPV_ARGS(&pso)));
	PSO = std::make_unique<PipelineState>(device, std::move(pso));
	constants = std::make_unique<BufferContainer<AreaReduceConstant>>(device, 1, L"Area Reduce Constants");
}
RenderGraphPass& AreaReducePass::insert_clear(RenderGraph& rg, AreaReducePassHandles const& handles) {
	return rg.add_pass(L"Area Reduce Clear")
		.readwrite(*handles.selection_uav.first)
		.execute([=](RgContext& ctx) {
		auto* r_out = ctx.graph->get<Buffer>(*handles.selection_uav.first);
		auto* r_out_uav = ctx.graph->get<UnorderedAccessView>(*handles.selection_uav.second);
		auto native = ctx.cmd->GetNativeCommandList();
		auto uav = r_out_uav->allocate_transient_descriptor(ctx.cmd);
		const uint clearValue[4] = {-1,-1,-1,-1};
		native->ClearUnorderedAccessViewUint(uav.get_gpu_handle(), uav.get_cpu_handle(), r_out->GetNativeResource(), clearValue, 0, NULL);
	});
}
RenderGraphPass& AreaReducePass::insert_reduce_material_instance(RenderGraph& rg, AreaReducePassHandles const& handles, uint2 point, uint2 extent) {
	return rg.add_pass(L"Area Reduce Unique Instances")
		.read(*handles.material_srv.first)
		.read(*handles.selection_uav.first)
		.readwrite(*handles.selection_uav.first)
		.execute([=](RgContext& ctx) {
			auto* r_texture = ctx.graph->get<Texture>(*handles.material_srv.first);
			auto* r_in_srv = ctx.graph->get<ShaderResourceView>(*handles.material_srv.second);
			auto* r_out_uav = ctx.graph->get<UnorderedAccessView>(*handles.selection_uav.second);
			uint width = r_texture->GetDesc().width, height = r_texture->GetDesc().height;
			auto native = ctx.cmd->GetNativeCommandList();
			native->SetPipelineState(*PSO);
			native->SetComputeRootSignature(*EditorGlobals::g_RHI.rootSig);
			constants->Data()->sourceSrv = r_in_srv->allocate_transient_descriptor(ctx.cmd).get_heap_handle();
			constants->Data()->outBufferUav = r_out_uav->allocate_transient_descriptor(ctx.cmd).get_heap_handle();
			constants->Data()->position = point;
			constants->Data()->extent = extent;
			constants->Data()->sourceDimension.x = width;
			constants->Data()->sourceDimension.y = height;
			native->SetComputeRootConstantBufferView(RHIContext::ROOTSIG_CB_SHADER_GLOBAL, constants->GetGPUAddress());
			native->Dispatch(DivRoundUp(extent.x, RENDERER_FULLSCREEN_THREADS), DivRoundUp(extent.y, RENDERER_FULLSCREEN_THREADS), 1);
		});
}