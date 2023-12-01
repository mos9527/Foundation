#include "Skybox.hpp"
#include "../Data/CubeMesh.hpp"

using namespace RHI;
SkyboxPass::SkyboxPass(RHI::Device* device) : device(device) {
	PS = BuildShader(L"SkyboxPass", L"ps_main", L"ps_6_6");
	VS = BuildShader(L"SkyboxPass", L"vs_main", L"vs_6_6");
	RS = std::make_unique<RootSignature>(
		device,
		RootSignatureDesc()
		.SetAllowInputAssembler()
		.SetDirectlyIndexed()
		.AddConstantBufferView(0, 0) // b0 space0 : SceneGlobals
		.AddStaticSampler(0, 0) // s0 space0 : Cubemap Sampler
	);
	RS->SetName(L"Skybox");
	// Describe and create the graphics pipeline state objects (PSO).
	D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
	auto iaLayout = VertexLayoutToD3DIADesc({{ "POSITION" ,RHI::ResourceFormat::R32G32B32_FLOAT }});	
	psoDesc.InputLayout = { iaLayout.data(), (UINT)iaLayout.size() };
	psoDesc.pRootSignature = *RS;
	psoDesc.VS = CD3DX12_SHADER_BYTECODE(VS->GetData(), VS->GetSize());
	psoDesc.PS = CD3DX12_SHADER_BYTECODE(PS->GetData(), PS->GetSize());
	psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	psoDesc.DepthStencilState.DepthEnable = TRUE;
	psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO; // No depth write
#ifdef INVERSE_Z
	psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
#else
	psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_LESS;
#endif
	psoDesc.SampleMask = UINT_MAX;
	psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	psoDesc.NumRenderTargets = 1;
	psoDesc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT; // Framebuffer
	psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	psoDesc.SampleDesc.Count = 1;
	ComPtr<ID3D12PipelineState> pso;
	CHECK_HR(device->GetNativeDevice()->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pso)));
	PSO = std::make_unique<PipelineState>(device, std::move(pso));
	// Prepare VB,IB for a cube
	// Vertex is float3
	VB = std::make_unique<Buffer>(device, Resource::ResourceDesc::GetGenericBufferDesc(
		sizeof(float3) * 8, sizeof(float3), ResourceState::Common, ResourceHeapType::Default,
		ResourceFlags::None, L"Cube VB"
	));
	IB = std::make_unique<Buffer>(device, Resource::ResourceDesc::GetGenericBufferDesc(
		sizeof(UINT16) * 36, sizeof(UINT16), ResourceState::Common, ResourceHeapType::Default,
		ResourceFlags::None, L"Cube IB"
	));
	// Upload the data
	UploadCubeMesh();
}
void SkyboxPass::UploadCubeMesh() {
	device->Wait();
	UploadContext ctx(device);	
	ctx.Begin();
	ctx.QueueUploadBuffer(VB.get(), (void*)&cube_vertices, sizeof(cube_vertices));
	ctx.QueueUploadBuffer(IB.get(), (void*)&cube_indices, sizeof(cube_indices));
	ctx.End().Wait();	
}
RenderGraphPass& SkyboxPass::insert(RenderGraph& rg, SceneView* sceneView, SkyboxPassHandles&& handles) {
	return rg.add_pass(L"Skybox")		
		.read(handles.frameBuffer)
		.write(handles.frameBuffer)
		.read(handles.depth)		
		.execute([=](RgContext& ctx) {			
			auto* r_fb_rtv = ctx.graph->get<RenderTargetView>(handles.frameBufferRTV);
			auto* r_depth_dsv = ctx.graph->get<DepthStencilView>(handles.depthDSV);
			auto* r_depth = ctx.graph->get<Texture>(handles.depth);
			UINT width = sceneView->get_SceneGlobals().frameDimension.x, height = sceneView->get_SceneGlobals().frameDimension.y;
			CD3DX12_VIEWPORT viewport(.0f, .0f, width, height, .0f, 1.0f);
			CD3DX12_RECT scissorRect(0, 0, width, height);			
			auto native = ctx.cmd->GetNativeCommandList();
			native->SetPipelineState(*PSO);
			native->SetGraphicsRootSignature(*RS);
			native->SetGraphicsRootConstantBufferView(0, sceneView->get_SceneGlobalsBuffer()->GetGPUAddress());			
			native->RSSetViewports(1, &viewport);
			native->RSSetScissorRects(1, &scissorRect);
			native->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			ctx.cmd->QueueTransitionBarrier(r_depth, ResourceState::DepthRead);
			ctx.cmd->FlushBarriers();
			native->OMSetRenderTargets(
				1,
				&r_fb_rtv->descriptor.get_cpu_handle(),
				FALSE,
				&r_depth_dsv->descriptor.get_cpu_handle()
			);
			const D3D12_VERTEX_BUFFER_VIEW vbView{
				VB->GetGPUAddress(),
				(UINT)VB->GetDesc().width,
				(UINT)VB->GetDesc().stride
			};			
			native->IASetVertexBuffers(0, 1, &vbView);
			const D3D12_INDEX_BUFFER_VIEW ibView{
				IB->GetGPUAddress(),
				(UINT)IB->GetDesc().width,
				DXGI_FORMAT_R16_UINT
			};
			native->IASetIndexBuffer(&ibView);
			native->DrawIndexedInstanced(36, 1, 0, 0, 0);
		});
}