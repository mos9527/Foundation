#include "HiZ.hpp"
using namespace RHI;
#ifdef INVERSE_Z // The closer the greater the depth. Do min reduction to avoid over-occluding geometries
#define REDUCTION_FUNCTION L"min(min(min(v0,v1), v2), v3)"
#else
#define REDUCTION_FUNCTION L"max(max(max(v0,v1), v2), v3)" // same goes for this except depth is reversed
#endif // INVERSE_Z
void HierarchalDepthPass::setup() {
	CS = BuildShader(L"DepthSampleToTexture", L"main", L"cs_6_6");
	D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};
	computePsoDesc.pRootSignature = *EditorGlobals::g_RHI.rootSig;
	computePsoDesc.CS = CD3DX12_SHADER_BYTECODE(CS->GetData(), CS->GetSize());
	ComPtr<ID3D12PipelineState> pso;
	CHECK_HR(device->GetNativeDevice()->CreateComputePipelineState(&computePsoDesc, IID_PPV_ARGS(&pso)));
	PSO = std::make_unique<PipelineState>(device, std::move(pso));
	constants = std::make_unique<BufferContainer<DepthSampleToTextureConstant>>(device, 1, L"Depth Sample Constants");
	
	spdPass.reduce_func = REDUCTION_FUNCTION;
	spdPass.setup();
}

RenderGraphPass& HierarchalDepthPass::insert(RenderGraph& rg, SceneView* sceneView, Handles const& handles) {
	rg.add_pass(L"Hierarchal Depth Sample To Texture")
		.read(handles.depth)
		.readwrite(handles.hizTexture)
		.execute([=](RgContext& ctx) -> void {
			auto* r_depth = ctx.graph->get<Texture>(handles.depth);
			auto* r_depth_srv = ctx.graph->get<ShaderResourceView>(handles.depthSRV);
			auto* r_hiz = ctx.graph->get<Texture>(handles.hizTexture);
			CHECK(handles.hizUAVs.size() == r_hiz->GetDesc().mipLevels);
			auto* r_hiz_uav = ctx.graph->get<UnorderedAccessView>(handles.hizUAVs[0]);
			DepthSampleToTextureConstant data{
				.depthSRVHeapIndex = r_depth_srv->descriptor.get_heap_handle(),
				.hizMip0UavHeapIndex = r_hiz_uav->descriptor.get_heap_handle(),
				.dimensions = uint2{ (uint)r_depth->GetDesc().width, (uint)r_depth->GetDesc().height },
			};
			constants->Update(&data, sizeof(data), 0);
			auto native = ctx.cmd->GetNativeCommandList();
			native->SetPipelineState(*PSO);
			native->SetComputeRootSignature(*EditorGlobals::g_RHI.rootSig);
			native->SetComputeRootConstantBufferView(RHIContext::ROOTSIG_CB_SHADER_GLOBAL, constants->GetGPUAddress());
			native->Dispatch(DivRoundUp(data.dimensions.x, RENDERER_FULLSCREEN_THREADS), DivRoundUp(data.dimensions.y, RENDERER_FULLSCREEN_THREADS), 1);
		});
	return spdPass.insert(rg, {
		.srcTexture = handles.hizTexture,
		.dstTexture = handles.hizTexture,
		.dstMipUAVs = handles.hizUAVs
	});
}