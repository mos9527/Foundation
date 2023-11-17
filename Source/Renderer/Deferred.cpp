#include "Deferred.hpp"
#include "../Engine/AssetRegistry/IO.hpp"
using namespace RHI;
DeferredRenderer::DeferredRenderer(AssetRegistry& assets, SceneGraph& scene, RHI::Device* device, RHI::Swapchain* swapchain) : Renderer(assets, scene, device, swapchain) {
	sceneView = std::make_unique<SceneGraphView>(device, scene);
	// cull pass
	cullPassCS = std::make_unique<Shader>(L"Shaders/InstanceCull.hlsl", L"main", L"cs_6_6");
	cullPassRS = std::make_unique<RootSignature>(
		device,
		RootSignatureDesc()			
			.AddConstantBufferView(0,0) // c0 s0 : SceneGlobals	
			.AddShaderResourceView(0,0) // t0 s0 : SceneMeshInstance[s]
			.AddUnorderedAccessView(0,0)// u0 s0 : Indirect Commandlists
	);
	cullPassRS->SetName(L"Indirect Cull & LOD Classification");
	D3D12_COMPUTE_PIPELINE_STATE_DESC computePsoDesc = {};
	computePsoDesc.pRootSignature = *cullPassRS;
	computePsoDesc.CS = CD3DX12_SHADER_BYTECODE(cullPassCS->GetData(), cullPassCS->GetSize());
	ComPtr<ID3D12PipelineState> pso;
	CHECK_HR(device->GetNativeDevice()->CreateComputePipelineState(&computePsoDesc, IID_PPV_ARGS(&pso)));
	cullPassPSO = std::make_unique<PipelineState>(device, pso);	
	// gbuffer pass
	gBufferPS = std::make_unique<Shader>(L"Shaders/GBuffer.hlsl", L"ps_main", L"ps_6_6");
	gBufferVS = std::make_unique<Shader>(L"Shaders/GBuffer.hlsl", L"vs_main", L"vs_6_6");
	gBufferRS = std::make_unique<RootSignature>(
		device,
		RootSignatureDesc()
		.SetDirectlyIndexed() // xxx textures?
		.AddConstantBufferView(0, 0) // c0 s0 : SceneGlobals			
	);
	gBufferRS->SetName(L"GBuffer generation");
	// Define the vertex input layout.
	auto iaLayout = VertexLayoutToD3DIADesc(StaticVertex::get_layout());
	// Describe and create the graphics pipeline state objects (PSO).
	D3D12_GRAPHICS_PIPELINE_STATE_DESC gbufferPsoDesc = {};
	gbufferPsoDesc.InputLayout = { iaLayout.data(), (UINT)iaLayout.size() };
	gbufferPsoDesc.pRootSignature = *gBufferRS;
	gbufferPsoDesc.VS = CD3DX12_SHADER_BYTECODE(gBufferVS->GetData(), gBufferVS->GetSize());
	gbufferPsoDesc.PS = CD3DX12_SHADER_BYTECODE(gBufferPS->GetData(), gBufferPS->GetSize());
	gbufferPsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	gbufferPsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	gbufferPsoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	gbufferPsoDesc.SampleMask = UINT_MAX;
	gbufferPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	gbufferPsoDesc.NumRenderTargets = 1;
	gbufferPsoDesc.RTVFormats[0] = ResourceFormatToD3DFormat(swapchain->GetFormat());
	gbufferPsoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	gbufferPsoDesc.SampleDesc.Count = 1;
	CHECK_HR(device->GetNativeDevice()->CreateGraphicsPipelineState(&gbufferPsoDesc, IID_PPV_ARGS(&pso)));
	gBufferPSO = std::make_unique<PipelineState>(device, pso);
	// indirect command buffer
	gBufferIndirectCommandSig = std::make_unique<CommandSignature>(
		device,
		CommandSignatureDesc(sizeof(IndirectCommand))
			.AddVertexBufferView(0)
			.AddIndexBufferView()
			.AddDrawIndexed()			
	);
	indirectCmdBuffer = std::make_unique<Buffer>(device, Resource::ResourceDesc::GetGenericBufferDesc(
		CommandBufferSize, sizeof(IndirectCommand), ResourceState::Common, ResourceHeapType::Default, ResourceFlags::UnorderedAccess
	));
	indirectCmdBufferUAV = std::make_unique<UnorderedAccessView>(
		indirectCmdBuffer.get(), UnorderedAccessViewDesc::GetStructuredBufferDesc(
			0, MAX_INSTANCE_COUNT, sizeof(IndirectCommand), CommandBufferCounterOffset
		)
	);
};
void DeferredRenderer::Render() {
	RenderGraph rg(cache);
	scene.get_active_camera().aspect = swapchain->GetAspect();
	sceneView->update();
	
}