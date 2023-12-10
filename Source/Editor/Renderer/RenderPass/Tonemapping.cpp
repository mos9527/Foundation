#include "Tonemapping.hpp"
using namespace RHI;
using namespace EditorGlobals;
void TonemappingPass::reset() {
	CS_Histogram = BuildShader(L"Tonemapping", L"main_histogram", L"cs_6_6");
	CS_Avg = BuildShader(L"Tonemapping", L"main_avg", L"cs_6_6");
	CS_Tonemap = BuildShader(L"Tonemapping", L"main_tonemap", L"cs_6_6");	
	D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};
	computePsoDesc.pRootSignature = *g_RHI.rootSig;
	
	ComPtr<ID3D12PipelineState> pso;
	computePsoDesc.CS = CD3DX12_SHADER_BYTECODE(CS_Histogram->GetData(), CS_Histogram->GetSize());
	CHECK_HR(device->GetNativeDevice()->CreateComputePipelineState(&computePsoDesc, IID_PPV_ARGS(&pso)));
	PSO_Histogram = std::make_unique<PipelineState>(device, std::move(pso));

	computePsoDesc.CS = CD3DX12_SHADER_BYTECODE(CS_Avg->GetData(), CS_Avg->GetSize());
	CHECK_HR(device->GetNativeDevice()->CreateComputePipelineState(&computePsoDesc, IID_PPV_ARGS(&pso)));
	PSO_Avg = std::make_unique<PipelineState>(device, std::move(pso));

	computePsoDesc.CS = CD3DX12_SHADER_BYTECODE(CS_Tonemap->GetData(), CS_Tonemap->GetSize());
	CHECK_HR(device->GetNativeDevice()->CreateComputePipelineState(&computePsoDesc, IID_PPV_ARGS(&pso)));
	PSO_Tonemap = std::make_unique<PipelineState>(device, std::move(pso));
	constexpr float histogramSize = 256;
	histogramBuffer = std::make_unique<Buffer>(device, Resource::ResourceDesc::GetGenericBufferDesc(
		histogramSize * sizeof(float), 0, ResourceState::UnorderedAccess, ResourceHeapType::Default,
		ResourceFlags::UnorderedAccess
	));
	histogramBufferUAV = std::make_unique<UnorderedAccessView>(histogramBuffer.get(), UnorderedAccessViewDesc::GetRawBufferDesc(0, histogramSize, 0));
	// Stores scene average luminance
	luminanceBuffer = std::make_unique<Texture>(device, Resource::ResourceDesc::GetTextureBufferDesc(
		ResourceFormat::R32_FLOAT, ResourceDimension::Texture2D,
		1, 1, 1, 1, 1, 0, ResourceFlags::UnorderedAccess, ResourceHeapType::Default, ResourceState::UnorderedAccess, {},
		L"Average Luminace"
	));
	luminanceBufferUAV = std::make_unique<UnorderedAccessView>(luminanceBuffer.get(), UnorderedAccessViewDesc::GetTexture2DDesc(
		ResourceFormat::R32_FLOAT, 0, 0
	));
	constants = std::make_unique<BufferContainer<TonemappingConstants>>(device, 1, L"Tonemapping Constants");
}

RenderGraphPass& TonemappingPass::insert(RenderGraph& rg, SceneView* sceneView, Handles const& handles) {
	return rg.add_pass(L"Tonemapping")
		.read(*handles.frameBuffer_uav.first) // Ensures....clear. ugh
		.read(*handles.frameBuffer_uav.first)
		.execute([=](RgContext& ctx) {
			UINT width = g_Editor.render.width, height = g_Editor.render.height;
			auto native = ctx.cmd->GetNativeCommandList();

			auto* r_fb_uav = ctx.graph->get<UnorderedAccessView>(*handles.frameBuffer_uav.second);			
			constants->Data()->framebufferUav = r_fb_uav->descriptor.get_heap_handle();
			constants->Data()->hisotrgramUav = histogramBufferUAV->descriptor.get_heap_handle();
			constants->Data()->avgLumUav = luminanceBufferUAV->descriptor.get_heap_handle();

			native->SetPipelineState(*PSO_Histogram);
			native->SetComputeRootSignature(*g_RHI.rootSig);		
			native->SetComputeRootConstantBufferView(RHIContext::ROOTSIG_CB_EDITOR_GLOBAL, sceneView->get_editor_globals_buffer()->GetGPUAddress());
			native->SetComputeRootConstantBufferView(RHIContext::ROOTSIG_CB_SHADER_GLOBAL, constants->GetGPUAddress());
			native->Dispatch(DivRoundUp(width, RENDERER_TONEMAPPING_THREADS), DivRoundUp(height, RENDERER_TONEMAPPING_THREADS), 1);
			ctx.cmd->QueueUAVBarrier(histogramBuffer.get());
			ctx.cmd->FlushBarriers();
			native->SetPipelineState(*PSO_Avg);
			native->SetComputeRootSignature(*g_RHI.rootSig);			
			native->SetComputeRootConstantBufferView(RHIContext::ROOTSIG_CB_EDITOR_GLOBAL, sceneView->get_editor_globals_buffer()->GetGPUAddress());
			native->SetComputeRootConstantBufferView(RHIContext::ROOTSIG_CB_SHADER_GLOBAL, constants->GetGPUAddress());
			native->Dispatch(1, 1, 1);
			ctx.cmd->QueueUAVBarrier(luminanceBuffer.get());
			ctx.cmd->FlushBarriers();
			native->SetPipelineState(*PSO_Tonemap);
			native->SetComputeRootSignature(*g_RHI.rootSig);			
			native->SetComputeRootConstantBufferView(RHIContext::ROOTSIG_CB_EDITOR_GLOBAL, sceneView->get_editor_globals_buffer()->GetGPUAddress());
			native->SetComputeRootConstantBufferView(RHIContext::ROOTSIG_CB_SHADER_GLOBAL, constants->GetGPUAddress());
			native->Dispatch(DivRoundUp(width, RENDERER_FULLSCREEN_THREADS), DivRoundUp(height, RENDERER_FULLSCREEN_THREADS), 1);
		});;
}