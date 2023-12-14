#include "IBLPrefilter.hpp"
using namespace RHI;
using namespace EditorGlobals;
IBLPrefilterPass::IBLPrefilterPass(RHI::Device* device) : spdPass(device) {
	Probe2CubemapCS = BuildShader(L"HDRIProbe", L"main_pano2cube", L"cs_6_6");
	PrefilterCS = BuildShader(L"HDRIProbe", L"main_prefilter", L"cs_6_6");

	D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};
	computePsoDesc.pRootSignature = *g_RHI.rootSig;
	ComPtr<ID3D12PipelineState> pso;	
	computePsoDesc.CS = CD3DX12_SHADER_BYTECODE(Probe2CubemapCS->GetData(), Probe2CubemapCS->GetSize());
	CHECK_HR(device->GetNativeDevice()->CreateComputePipelineState(&computePsoDesc, IID_PPV_ARGS(&pso)));
	Probe2CubemapPSO = std::make_unique<PipelineState>(device, std::move(pso));	
	computePsoDesc.CS = CD3DX12_SHADER_BYTECODE(PrefilterCS->GetData(), PrefilterCS->GetSize());
	CHECK_HR(device->GetNativeDevice()->CreateComputePipelineState(&computePsoDesc, IID_PPV_ARGS(&pso)));
	PrefilterPSO = std::make_unique<PipelineState>(device, std::move(pso));	

	constants = std::make_unique<BufferContainer<IBLPrefilterConstant>>(device, 1, L"Prefilter Constants");
};
// Samples HDRIProbeProcessor probe image to Cubemap, and performs downsampling to cover all mips
RenderGraphPass& IBLPrefilterPass::insert_pano2cube(RenderGraph& rg, IBLPrefilterPassHandles const& handles) {
	rg.add_pass(L"HDRI Probe To Cubemap")
		.readwrite(handles.cubemap)
		.execute([=](RgContext& ctx) {
		auto* r_hdri_srv = ctx.graph->get<ShaderResourceView>(handles.panoSrv);
		auto* r_cubemap_uav = ctx.graph->get<UnorderedAccessView>(*handles.cubemapUAVs[0]);
		auto* r_cubemap = ctx.graph->get<Texture>(handles.cubemap);
		uint dimension = r_cubemap->GetDesc().width;
		auto native = ctx.cmd->GetNativeCommandList();
		native->SetPipelineState(*Probe2CubemapPSO);
		native->SetComputeRootSignature(*g_RHI.rootSig);
		constants->Data()->hdriSourceSrv = r_hdri_srv->allocate_online_descriptor().get_heap_handle();
		constants->Data()->hdriDestUav = r_cubemap_uav->allocate_online_descriptor().get_heap_handle();
		constants->Data()->hdriDestDimension = dimension;
		native->SetComputeRootConstantBufferView(RHIContext::ROOTSIG_CB_SHADER_GLOBAL, constants->GetGPUAddress());
		native->Dispatch(DivRoundUp(dimension, RENDERER_FULLSCREEN_THREADS), DivRoundUp(dimension, RENDERER_FULLSCREEN_THREADS), 1);
	});
	return spdPass.insert(rg, {
		.srcTexture = handles.cubemap,
		.dstTexture = handles.cubemap,
		.dstMipUAVs = handles.cubemapUAVs
	});	
}

RenderGraphPass& IBLPrefilterPass::insert_specular_prefilter(RenderGraph& rg, IBLPrefilterPassHandles const& handles, uint mipIndex, uint mipLevels, uint cubeIndex) {
	return rg.add_pass(L"Specular Importance Sample")
		.read(handles.cubemap)
		.readwrite(handles.radianceCubeArray)
		.execute([=](RgContext& ctx) {
			auto* r_cubemap_srv = ctx.graph->get<ShaderResourceView>(handles.cubemapSRV);
			auto* r_radiance_uav = ctx.graph->get<UnorderedAccessView>(*handles.radianceCubeArrayUAVs[cubeIndex * mipLevels + mipIndex]); // a mip of a cube's 6 faces
			auto* r_cubemap = ctx.graph->get<Texture>(handles.cubemap);			
			std::wstring event = std::format(L"Specular IS Filter Cube{} Mip{} UAV{}", cubeIndex, mipIndex, cubeIndex * mipLevels + mipIndex);
			ctx.cmd->BeginEvent(event.c_str());			
			auto native = ctx.cmd->GetNativeCommandList();
			uint destDimension = r_cubemap->GetDesc().width >> mipIndex;
			native->SetPipelineState(*PrefilterPSO);
			native->SetComputeRootSignature(*g_RHI.rootSig);
			constants->Data()->prefilterFlag = IBL_FILTER_FLAG_RADIANCE;
			constants->Data()->prefilterSourceSize = r_cubemap->GetDesc().width;
			constants->Data()->prefilterDestSize = destDimension;
			constants->Data()->prefilterSourceSrv = r_cubemap_srv->allocate_online_descriptor().get_heap_handle();
			constants->Data()->prefilterDestUav = r_radiance_uav->allocate_online_descriptor().get_heap_handle();
			constants->Data()->prefilterCubeIndex = cubeIndex;
			constants->Data()->prefilterCubeMipIndex = mipIndex;
			native->SetComputeRootConstantBufferView(RHIContext::ROOTSIG_CB_SHADER_GLOBAL, constants->GetGPUAddress());
			native->Dispatch(DivRoundUp(destDimension, RENDERER_FULLSCREEN_THREADS), DivRoundUp(destDimension, RENDERER_FULLSCREEN_THREADS), 1);
			ctx.cmd->EndEvent();
		});
}
// Diffuse / irradiance sampling
// xxx spherical harmonics encoding
RenderGraphPass& IBLPrefilterPass::insert_diffuse_prefilter(RenderGraph& rg, IBLPrefilterPassHandles const& handles) {
	return rg.add_pass(L"Irradiance Importance Sample")
		.read(handles.cubemap)
		.readwrite(handles.irradianceCube)
		.execute([=](RgContext& ctx) {
			auto* r_cubemap_srv = ctx.graph->get<ShaderResourceView>(handles.cubemapSRV);
			auto* r_diffuse_uav = ctx.graph->get<UnorderedAccessView>(handles.irradianceCubeUAV);
			auto* r_cubemap = ctx.graph->get<Texture>(handles.cubemap);
			auto* r_irradiance = ctx.graph->get<Texture>(handles.irradianceCube);
			auto native = ctx.cmd->GetNativeCommandList();
			uint destDimension = r_irradiance->GetDesc().width;
			native->SetPipelineState(*PrefilterPSO);
			native->SetComputeRootSignature(*g_RHI.rootSig);
			constants->Data()->prefilterFlag = IBL_FILTER_FLAG_IRRADIANCE;
			constants->Data()->prefilterSourceSize = r_cubemap->GetDesc().width;
			constants->Data()->prefilterDestSize = destDimension;
			constants->Data()->prefilterSourceSrv = r_cubemap_srv->allocate_online_descriptor().get_heap_handle();
			constants->Data()->prefilterDestUav = r_diffuse_uav->allocate_online_descriptor().get_heap_handle();
			constants->Data()->prefilterCubeIndex = 0;
			constants->Data()->prefilterCubeMipIndex = 0;
			native->Dispatch(DivRoundUp(destDimension, RENDERER_FULLSCREEN_THREADS), DivRoundUp(destDimension, RENDERER_FULLSCREEN_THREADS), 1);
		});
};
