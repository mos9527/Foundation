#include "GBuffer.hpp"
using namespace RHI;
GBufferPass::GBufferPass(Device* device) {
	PS = BuildShader(L"GBuffer", L"ps_main", L"ps_6_6");
	VS = BuildShader(L"GBuffer", L"vs_main", L"vs_6_6");
	RS = std::make_unique<RootSignature>(
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
	RS->SetName(L"GBuffer generation");
	// Define the vertex input layout.
	auto iaLayout = VertexLayoutToD3DIADesc(MeshAsset::Vertex::get_layout());
	// Describe and create the graphics pipeline state objects (PSO).
	D3D12_GRAPHICS_PIPELINE_STATE_DESC gbufferPsoDesc = {};
	gbufferPsoDesc.InputLayout = { iaLayout.data(), (UINT)iaLayout.size() };
	gbufferPsoDesc.pRootSignature = *RS;
	gbufferPsoDesc.VS = CD3DX12_SHADER_BYTECODE(VS->GetData(), VS->GetSize());
	gbufferPsoDesc.PS = CD3DX12_SHADER_BYTECODE(PS->GetData(), PS->GetSize());
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
	gbufferPsoDesc.NumRenderTargets = 5; // ALBEDO[RGB+Mask], NORMAL, MATERIAL[metallic,roughness,[packed uint16]], EMISSIVE, VELOCITY
	gbufferPsoDesc.RTVFormats[0] = ResourceFormatToD3DFormat(ResourceFormat::R8G8B8A8_UNORM);
	gbufferPsoDesc.RTVFormats[1] = ResourceFormatToD3DFormat(ResourceFormat::R16G16_FLOAT); // see Shared.h
	gbufferPsoDesc.RTVFormats[2] = ResourceFormatToD3DFormat(ResourceFormat::R8G8B8A8_UNORM);
	gbufferPsoDesc.RTVFormats[3] = ResourceFormatToD3DFormat(ResourceFormat::R8G8B8A8_UNORM);
	gbufferPsoDesc.RTVFormats[4] = ResourceFormatToD3DFormat(ResourceFormat::R16G16_FLOAT);
	gbufferPsoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	gbufferPsoDesc.SampleDesc.Count = 1;
	ComPtr<ID3D12PipelineState> pso;
	CHECK_HR(device->GetNativeDevice()->CreateGraphicsPipelineState(&gbufferPsoDesc, IID_PPV_ARGS(&pso)));
	PSO = std::make_unique<PipelineState>(device, std::move(pso));
	// Wireframe PSO	
	gbufferPsoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
	CHECK_HR(device->GetNativeDevice()->CreateGraphicsPipelineState(&gbufferPsoDesc, IID_PPV_ARGS(&pso)));
	PSO_Wireframe = std::make_unique<PipelineState>(device, std::move(pso));
	// indirect command buffer
	IndirectCmdSig = std::make_unique<CommandSignature>(
		device,
		RS.get(),
		CommandSignatureDesc(sizeof(IndirectCommand))
		.AddConstant(0, 0, 2) // b0 space0 : MeshIndex,LodIndex constant (Indirect)
		.AddVertexBufferView(0)
		.AddIndexBufferView()
		.AddDrawIndexed()
	);
}

void GBufferPass::insert_execute(RenderGraphPass& pass, SceneView* sceneView, GBufferPassHandles&& handles, bool late) {
	pass.execute([=](RgContext& ctx) -> void {
		bool wireframe = sceneView->get_SceneGlobals().frameFlags & FRAME_FLAG_WIREFRAME;
		UINT width = sceneView->get_SceneGlobals().frameDimension.x, height = sceneView->get_SceneGlobals().frameDimension.y;
		// fetches from registry
		auto* r_albedo_rtv = ctx.graph->get<RenderTargetView>(handles.albedo_rtv);
		auto* r_normal_rtv = ctx.graph->get<RenderTargetView>(handles.normal_rtv);
		auto* r_material_rtv = ctx.graph->get<RenderTargetView>(handles.material_rtv);
		auto* r_emissive_rtv = ctx.graph->get<RenderTargetView>(handles.emissive_rtv);
		auto* r_velocity_rtv = ctx.graph->get<RenderTargetView>(handles.velocity_rtv);
		auto* r_dsv = ctx.graph->get<DepthStencilView>(handles.depth_dsv);
		auto* r_depthStencil = ctx.graph->get<Texture>(handles.depth);
		auto* r_indirect_commands = ctx.graph->get<Buffer>(handles.indirectCommands);
		auto* r_indirect_commands_uav = ctx.graph->get<UnorderedAccessView>(handles.indirectCommandsUAV);
		CD3DX12_VIEWPORT viewport(.0f, .0f, width, height, .0f, 1.0f);
		CD3DX12_RECT scissorRect(0, 0, width, height);

		auto native = ctx.cmd->GetNativeCommandList();
		auto setupPSO = [&]() {
			native->SetGraphicsRootSignature(*RS);
			// b0 used by indirect command
			native->SetGraphicsRootConstantBufferView(1, sceneView->get_SceneGlobalsBuffer()->GetGPUAddress());
			native->SetGraphicsRootShaderResourceView(2, sceneView->get_SceneMeshInstancesBuffer()->GetGPUAddress());
			native->SetGraphicsRootShaderResourceView(3, sceneView->get_SceneMeshMaterialsBuffer()->GetGPUAddress());
			native->RSSetViewports(1, &viewport);
			native->RSSetScissorRects(1, &scissorRect);
			native->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
		};
		if (wireframe){
			native->SetPipelineState(*PSO_Wireframe);
			setupPSO();
		} else {
			native->SetPipelineState(*PSO);
			setupPSO();
		}
		D3D12_CPU_DESCRIPTOR_HANDLE rtvHandles[5] = {
			r_albedo_rtv->descriptor,
			r_normal_rtv->descriptor,
			r_material_rtv->descriptor,
			r_emissive_rtv->descriptor,
			r_velocity_rtv->descriptor
		};		
		CHECK(r_indirect_commands_uav->GetDesc().HasCountedResource() && "Invalid Command Buffer!");
		ctx.cmd->Barrier(r_indirect_commands, ResourceState::IndirectArgument);
		ctx.cmd->FlushBarriers();
		native->OMSetRenderTargets(
			5,
			rtvHandles,
			FALSE,
			&r_dsv->descriptor.get_cpu_handle()
		);
		native->ExecuteIndirect(
			*IndirectCmdSig,
			MAX_INSTANCE_COUNT,
			r_indirect_commands->GetNativeResource(),
			0,
			r_indirect_commands->GetNativeResource(),
			r_indirect_commands_uav->GetDesc().GetCounterOffsetInBytes()
		);	
		if (wireframe) {
			native->SetPipelineState(*PSO);
			// Ensure Solid Depth is always rendered in the end regardless of wireframe flag
			// Useful for debugging some rendering features, like Occlusion Culling
			// PSO is in solid renderstate as of now:
			native->OMSetRenderTargets(
				0,
				NULL,
				FALSE,
				&r_dsv->descriptor.get_cpu_handle()
			);
			native->ExecuteIndirect(
				*IndirectCmdSig,
				MAX_INSTANCE_COUNT,
				r_indirect_commands->GetNativeResource(),
				0,
				r_indirect_commands->GetNativeResource(),
				r_indirect_commands_uav->GetDesc().GetCounterOffsetInBytes()
			);
		}
	});
}
// see IndirectLODCull
RenderGraphPass& GBufferPass::insert_earlydraw(RenderGraph& rg, SceneView* sceneView, GBufferPassHandles&& handles) {
	auto& pass = rg.add_pass(L"GBuffer Early Generation")
		.read(handles.indirectCommands)		
		.read(handles.depth)
		.read(handles.albedo).read(handles.normal).read(handles.material).read(handles.velocity).read(handles.emissive) // Ensures clear is complete
		.write(handles.depth)
		.write(handles.albedo).write(handles.normal).write(handles.material).write(handles.velocity).write(handles.emissive);
	insert_execute(pass, sceneView, std::move(handles),false /* late */);
	return pass;
}
RenderGraphPass& GBufferPass::insert_latedraw(RenderGraph& rg, SceneView* sceneView, GBufferPassHandles&& handles) {
	auto& pass = rg.add_pass(L"GBuffer Late Generation")
		.read(handles.indirectCommands)
		.write(handles.depth)
		.write(handles.albedo).write(handles.normal).write(handles.material).write(handles.velocity).write(handles.emissive);
	insert_execute(pass, sceneView, std::move(handles),true /* late */);
	return pass;
}