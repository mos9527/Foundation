#include "HiZ.hpp"
using namespace RHI;
#ifdef INVERSE_Z // The closer the greater the depth. Do min reduction to avoid over-occluding geometries
#define REDUCTION_FUNCTION L"min(min(min(v0,v1), v2), v3)"
#else
#define REDUCTION_FUNCTION L"max(max(max(v0,v1), v2), v3)" // same goes for this except depth is reversed
#endif // INVERSE_Z
HierarchalDepthPass::HierarchalDepthPass(Device* device) : spdPass(device, REDUCTION_FUNCTION) {
	CS = BuildShader(L"DepthSampleToTexture", L"main", L"cs_6_6");
	RS = std::make_unique<RootSignature>(
		device,
		RootSignatureDesc()
		.SetDirectlyIndexed()
		.AddConstantBufferView(0, 0) // b0 space0 : DepthSampleToTextureConstant
	);
	RS->SetName(L"Depth Sample to Texture");
	D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};
	computePsoDesc.pRootSignature = *RS;
	computePsoDesc.CS = CD3DX12_SHADER_BYTECODE(CS->GetData(), CS->GetSize());
	ComPtr<ID3D12PipelineState> pso;
	CHECK_HR(device->GetNativeDevice()->CreateComputePipelineState(&computePsoDesc, IID_PPV_ARGS(&pso)));
	PSO = std::make_unique<PipelineState>(device, std::move(pso));
	depthSampleConstants = std::make_unique<Buffer>(device, Resource::ResourceDesc::GetGenericBufferDesc(
		sizeof(DepthSampleToTextureConstant), sizeof(DepthSampleToTextureConstant)
	));
}

RenderGraphPass& HierarchalDepthPass::insert(RenderGraph& rg, SceneView* sceneView, HierarchalDepthPassHandles&& handles) {
	rg.add_pass(L"Hierarchal Depth Sample To Texture")
		.read(handles.depth)
		.readwrite(handles.hizTexture)
		.execute([=](RgContext& ctx) -> void {
			auto* r_depth = ctx.graph->get<Texture>(handles.depth);
			auto* r_depth_srv = ctx.graph->get<ShaderResourceView>(handles.depthSRV);
			auto* r_hiz = ctx.graph->get<Texture>(handles.hizTexture);
			CHECK(handles.hizUAVs.size() == r_hiz->GetDesc().mipLevels);
			auto* r_hiz_uav = ctx.graph->get<UnorderedAccessView>(handles.hizUAVs[0]);
			DepthSampleToTextureConstant constants{
				.depthSRVHeapIndex = r_depth_srv->descriptor.get_heap_handle(),
				.hizMip0UavHeapIndex = r_hiz_uav->descriptor.get_heap_handle(),
				.dimensions = uint2{ (uint)r_depth->GetDesc().width, (uint)r_depth->GetDesc().height },
			};
			depthSampleConstants->Update(&constants, sizeof(constants), 0);
			auto native = ctx.cmd->GetNativeCommandList();
			native->SetPipelineState(*PSO);
			native->SetComputeRootSignature(*RS);			
			native->SetComputeRootConstantBufferView(0, depthSampleConstants->GetGPUAddress());
			native->Dispatch(DivRoundUp(constants.dimensions.x, RENDERER_FULLSCREEN_THREADS), DivRoundUp(constants.dimensions.y, RENDERER_FULLSCREEN_THREADS), 1);
		});
	return spdPass.insert(rg, {
		.srcTexture = handles.hizTexture,
		.dstTexture = handles.hizTexture,
		.dstMipUAVs = handles.hizUAVs
	});
}