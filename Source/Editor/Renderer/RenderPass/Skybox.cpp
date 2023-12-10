#include "Skybox.hpp"
#include "../Data/CubeMesh.hpp"
using namespace RHI;
using namespace EditorGlobals;
void SkyboxPass::reset() {
	PS = BuildShader(L"SkyboxPass", L"ps_main", L"ps_6_6");
	VS = BuildShader(L"SkyboxPass", L"vs_main", L"vs_6_6");	
	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
	auto iaLayout = VertexLayoutToD3DIADesc({{ "POSITION" ,RHI::ResourceFormat::R32G32B32_FLOAT }});	
	desc.InputLayout = { iaLayout.data(), (UINT)iaLayout.size() };
	desc.pRootSignature = *g_RHI.rootSig;
	desc.VS = CD3DX12_SHADER_BYTECODE(VS->GetData(), VS->GetSize());
	desc.PS = CD3DX12_SHADER_BYTECODE(PS->GetData(), PS->GetSize());
	desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
	desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
	desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	desc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
	desc.DepthStencilState.DepthEnable = TRUE;
	desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO; // No depth write
#ifdef INVERSE_Z
	desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER_EQUAL;
#else
	desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_LESS;
#endif
	desc.SampleMask = UINT_MAX;
	desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	desc.NumRenderTargets = 1;
	desc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT; // Framebuffer
	desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	desc.SampleDesc.Count = 1;
	ComPtr<ID3D12PipelineState> pso;
	CHECK_HR(device->GetNativeDevice()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)));
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
	constants = std::make_unique<BufferContainer<SkyboxConstants>>(device, 1, "Skybox Constants");
}
void SkyboxPass::UploadCubeMesh() {
	device->Wait();
	UploadContext ctx(device);	
	ctx.Begin();
	ctx.QueueUploadBuffer(VB.get(), (void*)&cube_vertices, sizeof(cube_vertices));
	ctx.QueueUploadBuffer(IB.get(), (void*)&cube_indices, sizeof(cube_indices));
	ctx.End().Wait();	
}
RenderGraphPass& SkyboxPass::insert(RenderGraph& rg, SceneView* sceneView, Handles const& handles) {
	return rg.add_pass(L"Skybox")		
		.read(handles.frameBuffer)
		.write(handles.frameBuffer)
		.read(handles.depth)		
		.execute([=](RgContext& ctx) {			
			UINT width = g_Editor.render.width, height = g_Editor.render.height;
			auto* native = ctx.cmd->GetNativeCommandList();
			auto* r_fb_rtv = ctx.graph->get<RenderTargetView>(handles.frameBufferRTV);
			auto* r_depth_dsv = ctx.graph->get<DepthStencilView>(handles.depthDSV);
			auto* r_depth = ctx.graph->get<Texture>(handles.depth);
			CD3DX12_VIEWPORT viewport(.0f, .0f, width, height, .0f, 1.0f);
			CD3DX12_RECT scissorRect(0, 0, width, height);						

			constants->Data()->enabled = sceneView->get_shader_data().probe.use;
			constants->Data()->radianceHeapIndex = sceneView->get_shader_data().probe.probe->radianceCubeArraySRV->descriptor.get_heap_handle();
			constants->Data()->skyboxIntensity = sceneView->get_shader_data().probe.skyboxIntensity;
			constants->Data()->skyboxLod = sceneView->get_shader_data().probe.skyboxLod;

			native->SetPipelineState(*PSO);
			native->SetGraphicsRootSignature(*g_RHI.rootSig);
			native->SetGraphicsRootConstantBufferView(RHIContext::ROOTSIG_CB_EDITOR_GLOBAL, sceneView->get_editor_globals_buffer()->GetGPUAddress());
			native->SetGraphicsRootConstantBufferView(RHIContext::ROOTSIG_CB_SHADER_GLOBAL, constants->GetGPUAddress());
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