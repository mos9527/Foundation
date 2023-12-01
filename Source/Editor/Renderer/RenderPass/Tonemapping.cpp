#include "Tonemapping.hpp"
using namespace RHI;
TonemappingPass::TonemappingPass(Device* device) : device(device) {
	CS_Histogram = BuildShader(L"Tonemapping", L"main_histogram", L"cs_6_6");
	CS_Avg = BuildShader(L"Tonemapping", L"main_avg", L"cs_6_6");
	CS_Tonemap = BuildShader(L"Tonemapping", L"main_tonemap", L"cs_6_6");
	RS = std::make_unique<RootSignature>( // Draw the selected meshes' depth in viewspace. Similar to shadow mapping
		device,
		RootSignatureDesc()		
		.SetDirectlyIndexed()
		.AddConstant(0, 0, 5) // b0 space0 : width,height,Histogram UAV, Framebuffer UAV, Avg Lum. UAV
		.AddConstantBufferView(1, 0) // b1 space0 : SceneGlobals		
	);
	RS->SetName(L"Tonemap");
	D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};
	computePsoDesc.pRootSignature = *RS;
	
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
}

RenderGraphPass& TonemappingPass::insert(RenderGraph& rg, SceneView* sceneView, TonemappingPassHandles&& handles) {
	return rg.add_pass(L"Tonemapping")
		.read(handles.frameBuffer)
		.readwrite(handles.frameBuffer)
		.execute([=](RgContext& ctx) {
			UINT width = sceneView->get_SceneGlobals().frameDimension.x, height = sceneView->get_SceneGlobals().frameDimension.y;
			auto* r_fb_uav = ctx.graph->get<UnorderedAccessView>(handles.frameBufferUAV);
			UINT constants[5] = { width, height, histogramBufferUAV->descriptor.get_heap_handle(), r_fb_uav->descriptor.get_heap_handle(), luminanceBufferUAV->descriptor.get_heap_handle() };
			auto native = ctx.cmd->GetNativeCommandList();
			native->SetPipelineState(*PSO_Histogram);
			native->SetComputeRootSignature(*RS);			
			native->SetComputeRoot32BitConstants(0, 5, constants, 0);
			native->SetComputeRootConstantBufferView(1, sceneView->get_SceneGlobalsBuffer()->GetGPUAddress());
			native->Dispatch(DivRoundUp(width, RENDERER_TONEMAPPING_THREADS), DivRoundUp(height, RENDERER_TONEMAPPING_THREADS), 1);
			ctx.cmd->QueueUAVBarrier(histogramBuffer.get());
			ctx.cmd->FlushBarriers();
			native->SetPipelineState(*PSO_Avg);
			native->SetComputeRootSignature(*RS);
			native->SetComputeRoot32BitConstants(0, 4, constants, 0);
			native->SetComputeRootConstantBufferView(1, sceneView->get_SceneGlobalsBuffer()->GetGPUAddress());
			native->Dispatch(1, 1, 1);
			ctx.cmd->QueueUAVBarrier(luminanceBuffer.get());
			ctx.cmd->FlushBarriers();
			native->SetPipelineState(*PSO_Tonemap);
			native->SetComputeRootSignature(*RS);
			native->SetComputeRoot32BitConstants(0, 4, constants, 0);
			native->SetComputeRootConstantBufferView(1, sceneView->get_SceneGlobalsBuffer()->GetGPUAddress());
			native->Dispatch(DivRoundUp(width, RENDERER_FULLSCREEN_THREADS), DivRoundUp(height, RENDERER_FULLSCREEN_THREADS), 1);
		});;
}