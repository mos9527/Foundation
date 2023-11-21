#include "GBuffer.hpp"
using namespace RHI;
GBufferPass::GBufferPass(Device* device) {
	gBufferPS = std::make_unique<Shader>(L"Shaders/GBuffer.hlsl", L"ps_main", L"ps_6_6");
	gBufferVS = std::make_unique<Shader>(L"Shaders/GBuffer.hlsl", L"vs_main", L"vs_6_6");
	gBufferRS = std::make_unique<RootSignature>(
		device,
		RootSignatureDesc()
		.SetAllowInputAssembler()
		.SetDirectlyIndexed()
		.AddConstant(0, 0, 2) // b0 space0 : MeshIndex,LodIndex constant (Indirect)
		.AddConstantBufferView(1, 0) // b1 space0 : SceneGlobals		
		.AddShaderResourceView(0, 0) // t0 space0 : SceneMeshInstances
		.AddShaderResourceView(1, 0) // t1 space0 : SceneMaterials
		.AddStaticSampler(0, 0) // s0 space0 : Texture Sampler
	);
	gBufferRS->SetName(L"GBuffer generation");
	// Define the vertex input layout.
	auto iaLayout = VertexLayoutToD3DIADesc(StaticMeshAsset::Vertex::get_layout());
	// Describe and create the graphics pipeline state objects (PSO).
	D3D12_GRAPHICS_PIPELINE_STATE_DESC gbufferPsoDesc = {};
	gbufferPsoDesc.InputLayout = { iaLayout.data(), (UINT)iaLayout.size() };
	gbufferPsoDesc.pRootSignature = *gBufferRS;
	gbufferPsoDesc.VS = CD3DX12_SHADER_BYTECODE(gBufferVS->GetData(), gBufferVS->GetSize());
	gbufferPsoDesc.PS = CD3DX12_SHADER_BYTECODE(gBufferPS->GetData(), gBufferPS->GetSize());
	gbufferPsoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);	
	gbufferPsoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	gbufferPsoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
#ifdef INVERSE_Z
	gbufferPsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER;
#else
	gbufferPsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_LESS;
#endif
	gbufferPsoDesc.SampleMask = UINT_MAX;
	gbufferPsoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	gbufferPsoDesc.NumRenderTargets = 4; // ALBEDO[RGB+Mask], NORMAL, MATERIAL[metallic,roughness,[packed uint16]], EMISSIVE
	gbufferPsoDesc.RTVFormats[0] = ResourceFormatToD3DFormat(ResourceFormat::R8G8B8A8_UNORM);
	gbufferPsoDesc.RTVFormats[1] = ResourceFormatToD3DFormat(ResourceFormat::R16G16_FLOAT); // see Shared.h
	gbufferPsoDesc.RTVFormats[2] = ResourceFormatToD3DFormat(ResourceFormat::R8G8B8A8_UNORM);
	gbufferPsoDesc.RTVFormats[3] = ResourceFormatToD3DFormat(ResourceFormat::R8G8B8A8_UNORM);
	gbufferPsoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	gbufferPsoDesc.SampleDesc.Count = 1;
	ComPtr<ID3D12PipelineState> pso;
	CHECK_HR(device->GetNativeDevice()->CreateGraphicsPipelineState(&gbufferPsoDesc, IID_PPV_ARGS(&pso)));
	gBufferPSO = std::make_unique<PipelineState>(device, std::move(pso));
	// Wireframe PSO
	gbufferPsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
	CHECK_HR(device->GetNativeDevice()->CreateGraphicsPipelineState(&gbufferPsoDesc, IID_PPV_ARGS(&pso)));
	gBufferPSOWireframe = std::make_unique<PipelineState>(device, std::move(pso));
	// indirect command buffer
	gBufferIndirectCommandSig = std::make_unique<CommandSignature>(
		device,
		gBufferRS.get(),
		CommandSignatureDesc(sizeof(IndirectCommand))
		.AddConstant(0, 0, 2) // b0 space0 : MeshIndex,LodIndex constant (Indirect)
		.AddVertexBufferView(0)
		.AddIndexBufferView()
		.AddDrawIndexed()
	);
}

RenderGraphPass& GBufferPass::insert(RenderGraph& rg, SceneGraphView* sceneView, GBufferPassHandles& handles) {
	return rg.add_pass(L"GBuffer Generation")
		.read(handles.indirectCommands)
		.write(handles.depth)
		.write(handles.albedo).write(handles.normal).write(handles.material).write(handles.emissive)
		.execute([=](RgContext& ctx) -> void {
			UINT width = sceneView->get_SceneGlobals().frameDimension.x, height = sceneView->get_SceneGlobals().frameDimension.y;
			// fetches from registry
			auto* r_albedo_rtv = ctx.graph->get<RenderTargetView>(handles.albedo_rtv);
			auto* r_normal_rtv = ctx.graph->get<RenderTargetView>(handles.normal_rtv);
			auto* r_material_rtv = ctx.graph->get<RenderTargetView>(handles.material_rtv);
			auto* r_emissive_rtv = ctx.graph->get<RenderTargetView>(handles.emissive_rtv);
			auto* r_dsv = ctx.graph->get<DepthStencilView>(handles.depth_dsv);
			auto* r_depthStencil = ctx.graph->get<Texture>(handles.depth);
			auto* r_indirect_commands = ctx.graph->get<Buffer>(handles.indirectCommands);
			auto* r_indirect_commands_uav = ctx.graph->get<UnorderedAccessView>(handles.indirectCommandsUAV);

			auto native = ctx.cmd->GetNativeCommandList();
			if (sceneView->get_SceneGlobals().frameFlags & FRAME_FLAG_WIREFRAME)
				native->SetPipelineState(*gBufferPSOWireframe);
			else 
				native->SetPipelineState(*gBufferPSO);
			native->SetGraphicsRootSignature(*gBufferRS);
			// b0 used by indirect command
			native->SetGraphicsRootConstantBufferView(1, sceneView->get_SceneGlobalsBuffer()->GetGPUAddress());
			native->SetGraphicsRootShaderResourceView(2, sceneView->get_SceneMeshInstancesBuffer()->GetGPUAddress());
			native->SetGraphicsRootShaderResourceView(3, sceneView->get_SceneMeshMaterialsBuffer()->GetGPUAddress());		
			CD3DX12_VIEWPORT viewport(.0f, .0f, width, height, .0f, 1.0f);
			CD3DX12_RECT scissorRect(0, 0, width, height);
			native->RSSetViewports(1, &viewport);
			native->RSSetScissorRects(1, &scissorRect);
			native->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			D3D12_CPU_DESCRIPTOR_HANDLE rtvHandles[4] = {
				r_albedo_rtv->descriptor,
				r_normal_rtv->descriptor,
				r_material_rtv->descriptor,
				r_emissive_rtv->descriptor
			};
			native->OMSetRenderTargets(
				4,
				rtvHandles,
				FALSE,
				&r_dsv->descriptor.get_cpu_handle()
			);
			native->ClearDepthStencilView(
				r_dsv->descriptor.get_cpu_handle(),
				D3D12_CLEAR_FLAG_DEPTH,
				r_depthStencil->GetClearValue().depthStencil.depth,
				r_depthStencil->GetClearValue().depthStencil.stencil,
				1, &scissorRect
			);
			const FLOAT clearColor[4] = { .0f ,.0f ,.0f ,.0f };
			native->ClearRenderTargetView(
				r_albedo_rtv->descriptor,
				clearColor,
				1, &scissorRect
			);
			native->ClearRenderTargetView(
				r_normal_rtv->descriptor,
				clearColor,
				1, &scissorRect
			);
			native->ClearRenderTargetView(
				r_material_rtv->descriptor,
				clearColor,
				1, &scissorRect
			);
			native->ClearRenderTargetView(
				r_emissive_rtv->descriptor,
				clearColor,
				1, &scissorRect
			);
			CHECK(r_indirect_commands_uav->GetDesc().HasCountedResource() && "Invalid Command Buffer!");
			r_indirect_commands->SetBarrier(ctx.cmd, ResourceState::IndirectArgument);
			native->ExecuteIndirect(
				*gBufferIndirectCommandSig,
				MAX_INSTANCE_COUNT,
				r_indirect_commands->GetNativeResource(),
				0,
				r_indirect_commands->GetNativeResource(),
				r_indirect_commands_uav->GetDesc().GetCounterOffsetInBytes()
			);
	});
}