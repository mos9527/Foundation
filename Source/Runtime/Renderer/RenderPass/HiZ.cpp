#include "HiZ.hpp"
using namespace RHI;

HierarchalDepthPass::HierarchalDepthPass(Device* device) : spdPass(device) {
	depthSampleCS = std::make_unique<Shader>(L"Shaders/DepthSampleToTexture.hlsl", L"main", L"cs_6_6");
	depthSampleRS = std::make_unique<RootSignature>(
		device,
		RootSignatureDesc()
		.SetDirectlyIndexed()
		.AddConstantBufferView(0, 0) // b0 space0 : DepthSampleToTextureConstant			
	);
	depthSampleRS->SetName(L"Depth Sample to Texture");
	D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};
	computePsoDesc.pRootSignature = *depthSampleRS;
	computePsoDesc.CS = CD3DX12_SHADER_BYTECODE(depthSampleCS->GetData(), depthSampleCS->GetSize());
	ComPtr<ID3D12PipelineState> pso;
	CHECK_HR(device->GetNativeDevice()->CreateComputePipelineState(&computePsoDesc, IID_PPV_ARGS(&pso)));
	depthSamplePSO = std::make_unique<PipelineState>(device, std::move(pso));
	depthSampleConstants = std::make_unique<Buffer>(device, Resource::ResourceDesc::GetGenericBufferDesc(
		sizeof(DepthSampleToTextureConstant), sizeof(DepthSampleToTextureConstant)
	));
}

RenderGraphPass& HierarchalDepthPass::insert(RenderGraph& rg, SceneGraphView* sceneView, HierarchalDepthPassHandles& handles) {
	rg.add_pass(L"Hierarchal Depth Sample To Texture")
		.read(handles.depth)
		.readwrite(handles.hizTexture)
		.execute([=](RgContext& ctx) -> void {
			auto* r_depth = ctx.graph->get<Texture>(handles.depth);
			auto* r_depth_srv = ctx.graph->get<ShaderResourceView>(handles.depthSRV);
			auto* r_hiz_uav = ctx.graph->get<UnorderedAccessView>(handles.hizUAVs[0]);
			DepthSampleToTextureConstant constants{
				.depthSRVHeapIndex = r_depth_srv->descriptor.get_heap_handle(),
				.hizMip0UavHeapIndex = r_hiz_uav->descriptor.get_heap_handle(),
				.dimensions = uint2{ (uint)r_depth->GetDesc().width, (uint)r_depth->GetDesc().height },
			};
			depthSampleConstants->Update(&constants, sizeof(constants), 0);
			auto native = ctx.cmd->GetNativeCommandList();
			native->SetPipelineState(*depthSamplePSO);
			native->SetComputeRootSignature(*depthSampleRS);			
			native->SetComputeRootConstantBufferView(0, depthSampleConstants->GetGPUAddress());
			native->Dispatch(DivRoundUp(constants.dimensions.x, RENDERER_FULLSCREEN_THREADS), DivRoundUp(constants.dimensions.y, RENDERER_FULLSCREEN_THREADS), 1);
		});
	FFXSPDPass::FFXSPDPassHandles spdHandles{
		.srcTexture = handles.hizTexture,
		.dstTexture = handles.hizTexture,
		.dstMipUAVs = handles.hizUAVs
	};
	return spdPass.insert(rg, sceneView, spdHandles);
}