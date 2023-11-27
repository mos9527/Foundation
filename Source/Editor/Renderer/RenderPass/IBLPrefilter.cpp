#include "IBLPrefilter.hpp"
using namespace RHI;
IBLPrefilterPass::IBLPrefilterPass(RHI::Device* device) : spdPass(device) {
	Probe2CubemapCS = BuildShader(L"HDRIProbe", L"main_pano2cube", L"cs_6_6");
	RS = std::make_unique<RootSignature>(
		device,
		RootSignatureDesc()
		.SetDirectlyIndexed()
		.AddConstant(0, 0, 4) // HDRI SRV, Cubemap UAV, Cubemap Width, Cubemap Height
		.AddStaticSampler(0, 0, SamplerDesc::GetTextureSamplerDesc(16)) // Pano HDRI sampler
	);
	RS->SetName(L"HDRI Probe");
	D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};
	computePsoDesc.pRootSignature = *RS;
	computePsoDesc.CS = CD3DX12_SHADER_BYTECODE(Probe2CubemapCS->GetData(), Probe2CubemapCS->GetSize());
	ComPtr<ID3D12PipelineState> pso;
	CHECK_HR(device->GetNativeDevice()->CreateComputePipelineState(&computePsoDesc, IID_PPV_ARGS(&pso)));
	Probe2CubemapPSO = std::make_unique<PipelineState>(device, std::move(pso));
};

RenderGraphPass& IBLPrefilterPass::insert(RenderGraph& rg, IBLPrefilterPassHandles&& handles) {
	auto& pass = rg.add_pass(L"HDRI Probe To Cubemap")
		.readwrite(handles.cubemapOut)
		.execute([=](RgContext& ctx) {
		auto* r_hdri_srv = ctx.graph->get<ShaderResourceView>(handles.panoSrv);
		auto* r_cubemap = ctx.graph->get<Texture>(handles.cubemapOut);
		auto* r_cubemap_uav = ctx.graph->get<UnorderedAccessView>(handles.cubemapOutUAV);		
		auto native = ctx.cmd->GetNativeCommandList();
		native->SetPipelineState(*Probe2CubemapPSO);
		native->SetComputeRootSignature(*RS);
		native->SetComputeRoot32BitConstant(0, r_hdri_srv->descriptor.get_heap_handle(), 0);
		native->SetComputeRoot32BitConstant(0, r_cubemap_uav->descriptor.get_heap_handle(), 1);
		native->SetComputeRoot32BitConstant(0, r_cubemap->GetDesc().width, 2);
		native->SetComputeRoot32BitConstant(0, r_cubemap->GetDesc().height, 3);
		native->Dispatch(DivRoundUp(r_cubemap->GetDesc().width, RENDERER_FULLSCREEN_THREADS), DivRoundUp(r_cubemap->GetDesc().height, RENDERER_FULLSCREEN_THREADS), 1);
	});
	spdPass.insert(rg, {
		.srcTexture = handles.cubemapOut,
		.dstTexture = handles.cubemapOut,
		.dstMipUAVs = handles.cubemapOutUAVs
	});
	rg.get_epilogue_pass().readwrite(handles.cubemapOut);
	return pass;
}
