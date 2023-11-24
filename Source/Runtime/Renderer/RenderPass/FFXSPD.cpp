#include "FFXSpd.hpp"
#define A_CPU
#include "../Shaders/ffx-spd/ffx_a.h"
#include "../Shaders/ffx-spd/ffx_spd.h"
using namespace RHI;
FFXSPDPass::FFXSPDPass(RHI::Device* device, const wchar_t* reduce) {
	CHECK(reduce && "Reduction function undefined.");
	std::wstring reduce_define = L"SPD_REDUCTION_FUNCTION=";
	reduce_define += reduce;
	CS = std::make_unique<Shader>(L"Shaders/FFXSpd.hlsl", L"main", L"cs_6_6", std::vector<const wchar_t*>{ reduce_define.c_str() });
	RS = std::make_unique<RootSignature>(
		device,
		RootSignatureDesc()
		.SetDirectlyIndexed()
		.AddConstantBufferView(0, 0) // b0 space0 : FFXSpdConstant
	);
	RS->SetName(L"FFX SPD Downsample");
	D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};
	computePsoDesc.pRootSignature = *RS;
	computePsoDesc.CS = CD3DX12_SHADER_BYTECODE(CS->GetData(), CS->GetSize());
	ComPtr<ID3D12PipelineState> pso;
	CHECK_HR(device->GetNativeDevice()->CreateComputePipelineState(&computePsoDesc, IID_PPV_ARGS(&pso)));
	PSO = std::make_unique<PipelineState>(device, std::move(pso));
	ffxPassConstants = std::make_unique<Buffer>(device, Buffer::ResourceDesc::GetGenericBufferDesc(sizeof(FFXSpdConstant), sizeof(FFXSpdConstant)));
	ffxPassCounter = std::make_unique<Buffer>(device, Buffer::ResourceDesc::GetGenericBufferDesc(
		sizeof(uint) * SPD_MAX_NUM_SLICES, 0 , ResourceState::CopyDest, ResourceHeapType::Default , ResourceFlags::UnorderedAccess
	));
	ffxPassCounterUAV = std::make_unique<UnorderedAccessView>(ffxPassCounter.get(), UnorderedAccessViewDesc::GetRawBufferDesc(0, SPD_MAX_NUM_SLICES, 0));
	// perform the initial clear
	auto* cmd = device->GetDefaultCommandList<CommandListType::Compute>();
	cmd->Begin();
	cmd->ZeroBufferRegion(ffxPassCounter.get(), 0, ffxPassCounter->GetDesc().sizeInBytes());
	cmd->Barrier(ffxPassCounter.get(), ResourceState::UnorderedAccess);
	cmd->FlushBarriers();
	cmd->Close();
	cmd->Execute().Wait();	
}
RenderGraphPass& FFXSPDPass::insert(RenderGraph& rg, SceneView* sceneView, FFXSPDPassHandles&& handles) {
	RenderGraphPass& pass = rg.add_pass(L"FFX SPD Downsample");
	pass.read(handles.srcTexture);
	pass.readwrite(handles.dstTexture);
	return pass.execute([=](RgContext& ctx) -> void {			
			auto* r_dst_texture = ctx.graph->get<Texture>(handles.dstTexture);
			auto* r_src_texture = ctx.graph->get<Texture>(handles.srcTexture);

			uint numMips = r_dst_texture->GetDesc().mipLevels;
			uint numSlices = r_dst_texture->GetDesc().arraySize;

			varAU2(dispatchThreadGroupCountXY);
			varAU2(workGroupOffset); // needed if Left and Top are not 0,0
			varAU2(numWorkGroupsAndMips);
			varAU4(rectInfo) = initAU4(0, 0, (UINT)r_dst_texture->GetDesc().width, (UINT)r_dst_texture->GetDesc().height); // left, top, width, height
			SpdSetup(dispatchThreadGroupCountXY, workGroupOffset, numWorkGroupsAndMips, rectInfo);
			
			FFXSpdConstant constants{};
			constants.atomicCounterHeapIndex = ffxPassCounterUAV->descriptor.get_heap_handle();
			constants.numWorkGroups = numWorkGroupsAndMips[0];
			constants.numMips = numWorkGroupsAndMips[1];
			CHECK(constants.numMips == numMips && constants.numMips == handles.dstMipUAVs.size() && "Bad mip count!");			
			for (uint i = 0; i < constants.numMips; i++) {
				UnorderedAccessView* r_uav = ctx.graph->get<UnorderedAccessView>(handles.dstMipUAVs[i]);
				constants.dstMipHeapIndex[i].x = r_uav->descriptor.get_heap_handle(); // for alignment uint4 is used to store one uint
			}
			constants.dimensions.x = rectInfo[2];
			constants.dimensions.y = rectInfo[3];
			ffxPassConstants->Update(&constants, sizeof(FFXSpdConstant), 0);
			if (r_src_texture != r_dst_texture) {
				// When different src-dst is used, MIP 0 is directly copied from src to dst
				auto src_original_state = r_src_texture->GetSubresourceState(0);
				ctx.cmd->Barrier(r_src_texture, ResourceState::CopySource, 0);
				ctx.cmd->Barrier(r_dst_texture, ResourceState::CopyDest, 0);
				ctx.cmd->FlushBarriers();
				ctx.cmd->CopySubresource(r_src_texture, r_src_texture->GetDesc().indexSubresource(0, 0, 0), r_dst_texture, r_dst_texture->GetDesc().indexSubresource(0, 0, 0));			
				ctx.cmd->Barrier(r_src_texture, src_original_state, 0);
				ctx.cmd->Barrier(r_dst_texture, ResourceState::UnorderedAccess, 0);
				ctx.cmd->FlushBarriers();
			}
			// otherwise, assume dst texture has its mip[0] set as downsample starting point
			auto native = ctx.cmd->GetNativeCommandList();
			native->SetPipelineState(*PSO);
			native->SetComputeRootSignature(*RS);
			native->SetComputeRootConstantBufferView(0, ffxPassConstants->GetGPUAddress());
			native->Dispatch(dispatchThreadGroupCountXY[0], dispatchThreadGroupCountXY[1], numSlices);
		});
}