#include "IBLPrefilter.hpp"
using namespace RHI;
IBLPrefilterPass::IBLPrefilterPass(RHI::Device* device) : spdPass(device) {
	HDRI2CubemapCS = BuildShader(L"HDRI2Cubemap", L"main", L"cs_6_6");
	HDRI2CubemapRS = std::make_unique<RootSignature>(
		device,
		RootSignatureDesc()
		.SetDirectlyIndexed()
		.AddConstant(0, 0, 4) // HDRI SRV, Cubemap UAV, Cubemap Width, Cubemap Height
		.AddStaticSampler(0, 0, SamplerDesc::GetTextureSamplerDesc(16)) // Pano HDRI sampler
	);
	HDRI2CubemapRS->SetName(L"Depth Sample to Texture");
	D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};
	computePsoDesc.pRootSignature = *HDRI2CubemapRS;
	computePsoDesc.CS = CD3DX12_SHADER_BYTECODE(HDRI2CubemapCS->GetData(), HDRI2CubemapCS->GetSize());
	ComPtr<ID3D12PipelineState> pso;
	CHECK_HR(device->GetNativeDevice()->CreateComputePipelineState(&computePsoDesc, IID_PPV_ARGS(&pso)));
	HDRI2CubemapPSO = std::make_unique<PipelineState>(device, std::move(pso));
};

RenderGraphPass& IBLPrefilterPass::insert(RenderGraph& rg, IBLPrefilterPassHandles&& handles) {
	auto& pass = rg.add_pass(L"HDRI To Cubemap")
		.readwrite(handles.cubemapOut)
		.execute([=](RgContext& ctx) {
		auto* r_hdri_srv = ctx.graph->get<ShaderResourceView>(handles.panoSrv);
		auto* r_cubemap = ctx.graph->get<Texture>(handles.cubemapOut);
		auto* r_cubemap_uav = ctx.graph->get<UnorderedAccessView>(handles.cubemapOutUAV);		
		auto native = ctx.cmd->GetNativeCommandList();
		native->SetPipelineState(*HDRI2CubemapPSO);
		native->SetComputeRootSignature(*HDRI2CubemapRS);
		native->SetComputeRoot32BitConstant(0, r_hdri_srv->descriptor.get_heap_handle(), 0);
		native->SetComputeRoot32BitConstant(0, r_cubemap_uav->descriptor.get_heap_handle(), 1);
		native->SetComputeRoot32BitConstant(0, r_cubemap->GetDesc().width, 2);
		native->SetComputeRoot32BitConstant(0, r_cubemap->GetDesc().height, 3);
		native->Dispatch(DivRoundUp(r_cubemap->GetDesc().width, RENDERER_FULLSCREEN_THREADS), DivRoundUp(r_cubemap->GetDesc().height, RENDERER_FULLSCREEN_THREADS), 1);
	});
	rg.get_epilogue_pass().read(handles.cubemapOut);
	return pass;
}
