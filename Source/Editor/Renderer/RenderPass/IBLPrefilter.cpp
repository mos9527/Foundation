#include "IBLPrefilter.hpp"
using namespace RHI;
IBLPrefilterPass::IBLPrefilterPass(RHI::Device* device) : spdPass(device) {
	Probe2CubemapCS = BuildShader(L"HDRIProbe", L"main_pano2cube", L"cs_6_6");
	PrefilterCS = BuildShader(L"HDRIProbe", L"main_prefilter", L"cs_6_6");
	LUTCS = BuildShader(L"HDRIProbe", L"main_lut", L"cs_6_6");
	RS = std::make_unique<RootSignature>(
		device,
		RootSignatureDesc()
		.SetDirectlyIndexed()
		.AddConstant(0, 0, 6) 
		//               0                  1              2              3                   4                    5
		// * pano2cube : Cubemap Dimension, HDRI SRV,      Cubemap UAV, *3 unused
		// * prefilter : Cubemap Dimension, Cubemap SRV,   Prefilter UAV, Dispatch Mip Index, Dispatch Cube Index, Filter Flags
		// * lut :       Cubemap Dimension, LUT Dimension, Cubemap SRV,   LUT Array UAV,      Filter Flags, *1 unused
		.AddStaticSampler(0, 0, SamplerDesc::GetTextureSamplerDesc(16))
	);
	RS->SetName(L"HDRI Probe");
	D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};
	computePsoDesc.pRootSignature = *RS;
	computePsoDesc.CS = CD3DX12_SHADER_BYTECODE(Probe2CubemapCS->GetData(), Probe2CubemapCS->GetSize());
	ComPtr<ID3D12PipelineState> pso;
	CHECK_HR(device->GetNativeDevice()->CreateComputePipelineState(&computePsoDesc, IID_PPV_ARGS(&pso)));
	Probe2CubemapPSO = std::make_unique<PipelineState>(device, std::move(pso));
	computePsoDesc.CS = CD3DX12_SHADER_BYTECODE(PrefilterCS->GetData(), PrefilterCS->GetSize());
	PrefilterPSO = std::make_unique<PipelineState>(device, std::move(pso));
	computePsoDesc.CS = CD3DX12_SHADER_BYTECODE(LUTCS->GetData(), LUTCS->GetSize());
	LUTPSO = std::make_unique<PipelineState>(device, std::move(pso));
};
// Samples HDRI probe image to Cubemap, and performs downsampling to cover all mips
RenderGraphPass& IBLPrefilterPass::insert_pano2cube(RenderGraph& rg, IBLPrefilterPassHandles& handles) {
	auto& pass = rg.add_pass(L"HDRI Probe To Cubemap")
		.readwrite(handles.cubemap)
		.execute([=](RgContext& ctx) {
		auto* r_hdri_srv = ctx.graph->get<ShaderResourceView>(handles.panoSrv);
		auto* r_cubemap_uav = ctx.graph->get<UnorderedAccessView>(handles.cubemapUAVs[0]);
		auto* r_cubemap = ctx.graph->get<Texture>(handles.cubemap);
		uint dimension = r_cubemap->GetDesc().width;
		auto native = ctx.cmd->GetNativeCommandList();
		native->SetPipelineState(*Probe2CubemapPSO);
		native->SetComputeRootSignature(*RS);
		native->SetComputeRoot32BitConstant(0, dimension, 0);		
		native->SetComputeRoot32BitConstant(0, r_hdri_srv->descriptor.get_heap_handle(), 1);
		native->SetComputeRoot32BitConstant(0, r_cubemap_uav->descriptor.get_heap_handle(), 2);
		native->Dispatch(DivRoundUp(dimension, RENDERER_FULLSCREEN_THREADS), DivRoundUp(dimension, RENDERER_FULLSCREEN_THREADS), 1);
	});
	spdPass.insert(rg, {
		.srcTexture = handles.cubemap,
		.dstTexture = handles.cubemap,
		.dstMipUAVs = handles.cubemapUAVs
	});
	rg.get_epilogue_pass().readwrite(handles.cubemap);
	return pass;
}

RenderGraphPass& IBLPrefilterPass::insert_specular_prefilter(RenderGraph& rg, IBLPrefilterPassHandles& handles, uint mipIndex, uint mipLevels, uint cubeIndex) {
	return rg.add_pass(L"Specular Importance Sample")
		.read(handles.cubemap)
		.readwrite(handles.radianceCubeArray)
		.execute([=](RgContext& ctx) {
			auto* r_cubemap_srv = ctx.graph->get<ShaderResourceView>(handles.cubemapSRV);
			auto* r_radiance_uav = ctx.graph->get<UnorderedAccessView>(handles.radianceCubeArrayUAVs[cubeIndex * mipLevels + mipIndex]); // a mip of a cube's 6 faces
			auto* r_cubemap = ctx.graph->get<Texture>(handles.cubemap);
			uint dimension = r_cubemap->GetDesc().width;
			auto native = ctx.cmd->GetNativeCommandList();
			native->SetPipelineState(*PrefilterPSO);
			native->SetComputeRootSignature(*RS);
			native->SetComputeRoot32BitConstant(0, dimension, 0);
			native->SetComputeRoot32BitConstant(0, r_cubemap_srv->descriptor.get_heap_handle(), 1);
			native->SetComputeRoot32BitConstant(0, r_radiance_uav->descriptor.get_heap_handle(), 2);
			native->SetComputeRoot32BitConstant(0, mipIndex, 3);
			native->SetComputeRoot32BitConstant(0, cubeIndex,4);			
			native->SetComputeRoot32BitConstant(0, IBL_FILTER_FLAG_RADIANCE, 5);
			native->Dispatch(DivRoundUp(dimension, RENDERER_FULLSCREEN_THREADS), DivRoundUp(dimension, RENDERER_FULLSCREEN_THREADS), 1);
		});
}
// Diffuse / irradiance sampling
RenderGraphPass& IBLPrefilterPass::insert_diffuse_prefilter(RenderGraph& rg, IBLPrefilterPassHandles& handles) {
	return rg.add_pass(L"Irradiance Importance Sample")
		.read(handles.cubemap)
		.readwrite(handles.irradianceCube)
		.execute([=](RgContext& ctx) {
			auto* r_cubemap_srv = ctx.graph->get<ShaderResourceView>(handles.cubemapSRV);
			auto* r_diffuse_uav = ctx.graph->get<UnorderedAccessView>(handles.irradianceCubeUAV);
			auto* r_cubemap = ctx.graph->get<Texture>(handles.cubemap);
			uint dimension = r_cubemap->GetDesc().width;
			auto native = ctx.cmd->GetNativeCommandList();
			native->SetPipelineState(*PrefilterPSO);
			native->SetComputeRootSignature(*RS);
			native->SetComputeRoot32BitConstant(0, dimension, 0);
			native->SetComputeRoot32BitConstant(0, r_cubemap_srv->descriptor.get_heap_handle(), 1);
			native->SetComputeRoot32BitConstant(0, r_diffuse_uav->descriptor.get_heap_handle(), 2);
			native->SetComputeRoot32BitConstant(0, IBL_FILTER_FLAG_IRRADIANCE, 5);
			native->Dispatch(DivRoundUp(dimension, RENDERER_FULLSCREEN_THREADS), DivRoundUp(dimension, RENDERER_FULLSCREEN_THREADS), 1);
		});
};
RenderGraphPass& IBLPrefilterPass::insert_lut(RenderGraph& rg, IBLPrefilterPassHandles& handles) {
	return rg.add_pass(L"Split Sum LUT")
		.read(handles.cubemap)
		.readwrite(handles.lutArray)
		.execute([=](RgContext& ctx) {
			auto* r_cubemap_srv = ctx.graph->get<ShaderResourceView>(handles.cubemapSRV);
			auto* r_lutarray_uav = ctx.graph->get<UnorderedAccessView>(handles.lutArrayUAV);
			auto* r_cubemap = ctx.graph->get<Texture>(handles.cubemap);
			auto* r_lut = ctx.graph->get<Texture>(handles.lutArray);
			uint dimension = r_lut->GetDesc().width;
			auto native = ctx.cmd->GetNativeCommandList();
			native->SetPipelineState(*LUTPSO);
			native->SetComputeRootSignature(*RS);
			native->SetComputeRoot32BitConstant(0, r_cubemap->GetDesc().width, 0);
			native->SetComputeRoot32BitConstant(0, dimension, 1);
			native->SetComputeRoot32BitConstant(0, r_cubemap_srv->descriptor.get_heap_handle(), 2);
			native->SetComputeRoot32BitConstant(0, r_lutarray_uav->descriptor.get_heap_handle(), 3);
			native->SetComputeRoot32BitConstant(0, IBL_FILTER_FLAG_IRRADIANCE, 4);
			native->Dispatch(DivRoundUp(dimension, RENDERER_FULLSCREEN_THREADS), DivRoundUp(dimension, RENDERER_FULLSCREEN_THREADS), 1);
		});
};
