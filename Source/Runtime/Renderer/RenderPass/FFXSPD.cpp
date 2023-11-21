#include "FFXSpd.hpp"
#define A_CPU
#include "../Shaders/ffx-spd/ffx_a.h"
#include "../Shaders/ffx-spd/ffx_spd.h"
using namespace RHI;
FFXSPDPass::FFXSPDPass(RHI::Device* device) {
	ffxPassCS = std::make_unique<Shader>(L"Shaders/FFXSpd.hlsl", L"main", L"cs_6_6");
	ffxPassRS = std::make_unique<RootSignature>(
		device,
		RootSignatureDesc()
		.SetDirectlyIndexed()
		.AddConstantBufferView(0, 0) // b0 space0 : FFXSpd Constants			
	);
	ffxPassRS->SetName(L"FFX SPD Downsample");
	D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};
	computePsoDesc.pRootSignature = *ffxPassRS;
	computePsoDesc.CS = CD3DX12_SHADER_BYTECODE(ffxPassCS->GetData(), ffxPassCS->GetSize());
	ComPtr<ID3D12PipelineState> pso;
	CHECK_HR(device->GetNativeDevice()->CreateComputePipelineState(&computePsoDesc, IID_PPV_ARGS(&pso)));
	ffxPassPSO = std::make_unique<PipelineState>(device, std::move(pso));
	ffxPassConstants = std::make_unique<Buffer>(device, Buffer::ResourceDesc::GetGenericBufferDesc(sizeof(FFXSpdConstant), sizeof(FFXSpdConstant)));
	ffxPassCounter = std::make_unique<Buffer>(device, Buffer::ResourceDesc::GetGenericBufferDesc(
		sizeof(uint) * SPD_MAX_NUM_SLICES, sizeof(uint) , ResourceState::CopyDest, ResourceHeapType::Default
	));
	ffxPassCounterUAV = std::make_unique<UnorderedAccessView>(ffxPassCounter.get(), UnorderedAccessViewDesc::GetRawBufferDesc(0, SPD_MAX_NUM_SLICES, 0));
	// perform the initial clear
	auto* cmd = device->GetCommandList<CommandListType::Compute>();
	cmd->Begin();
	cmd->ZeroBufferRegion(ffxPassCounter.get(), 0, ffxPassCounter->GetDesc().sizeInBytes());
	cmd->End();
	cmd->Execute().Wait();	
}
void FFXSPDPass::insert(RenderGraph& rg, SceneGraphView* sceneView, FFXSPDPassHandles& handles) {
	rg.add_pass(L"FFX SPD Downsample")
		.readwrite(handles.texture)
		.execute([=](RgContext& ctx) -> void {			
			auto* r_texture = ctx.graph->get<Texture>(handles.texture);
			uint numMips = r_texture->GetDesc().mipLevels;
			uint numSlices = r_texture->GetDesc().arraySize;

			varAU2(dispatchThreadGroupCountXY);
			varAU2(workGroupOffset); // needed if Left and Top are not 0,0
			varAU2(numWorkGroupsAndMips);
			varAU4(rectInfo) = initAU4(0, 0, (UINT)r_texture->GetDesc().width, (UINT)r_texture->GetDesc().height); // left, top, width, height
			SpdSetup(dispatchThreadGroupCountXY, workGroupOffset, numWorkGroupsAndMips, rectInfo);
			
			FFXSpdConstant constants;
			constants.atomicCounterHeapIndex = ffxPassCounterUAV->descriptor.get_heap_handle();
			constants.numWorkGroups = numWorkGroupsAndMips[0];
			constants.numMips = numWorkGroupsAndMips[1];
			CHECK(constants.numMips == numMips && constants.numMips == handles.mipUAVs.size() && "Bad mip count!");			
			for (uint i = 0; i < constants.numMips; i++) {
				UnorderedAccessView* r_uav = ctx.graph->get<UnorderedAccessView>(*handles.mipUAVs[i]);
				constants.mipViewHeapIndex[i].x = r_uav->descriptor.get_heap_handle(); // for alignment			
			}	
			auto native = ctx.cmd->GetNativeCommandList();
			native->SetPipelineState(*ffxPassPSO);
			native->SetComputeRootSignature(*ffxPassRS);
			native->SetComputeRootConstantBufferView(0, ffxPassConstants->GetGPUAddress());
			native->Dispatch(dispatchThreadGroupCountXY[0], dispatchThreadGroupCountXY[1], numSlices);
		});
}