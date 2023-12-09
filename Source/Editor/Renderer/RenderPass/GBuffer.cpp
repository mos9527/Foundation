/* Deferred Texturing GBuffer Implementation

* see https://therealmjp.github.io/posts/bindless-texturing-for-deferred-rendering-and-decals/
*/
#include "GBuffer.hpp"
using namespace RHI;
using namespace EditorGlobals;
void GBufferPass::setup() {
	PS = BuildShader(L"GBuffer", L"ps_main", L"ps_6_6");
	VS = BuildShader(L"GBuffer", L"vs_main", L"vs_6_6");
	auto iaLayout = VertexLayoutToD3DIADesc(StaticMeshAsset::Vertex::get_layout());
	D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
	desc.InputLayout = { iaLayout.data(), (UINT)iaLayout.size() };
	desc.pRootSignature = *g_RHI.rootSig;
	desc.VS = CD3DX12_SHADER_BYTECODE(VS->GetData(), VS->GetSize());
	desc.PS = CD3DX12_SHADER_BYTECODE(PS->GetData(), PS->GetSize());
	desc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
	desc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
#ifdef INVERSE_Z
	desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_GREATER;
#else
	gbufferPsoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_LESS;
#endif
	desc.SampleMask = UINT_MAX;
	desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
	desc.NumRenderTargets = 3;
	desc.RTVFormats[0] = ResourceFormatToD3DFormat(ResourceFormat::R10G10B10A2_UNORM);  // Packed Tangent Frame
	desc.RTVFormats[1] = ResourceFormatToD3DFormat(ResourceFormat::R16G16B16A16_SNORM); // Depth Gradient (reconstructs position gradient), UV Gradient
	desc.RTVFormats[2] = ResourceFormatToD3DFormat(ResourceFormat::R16G16B16A16_UNORM);	// Material ID, UV
	desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
	desc.SampleDesc.Count = 1;
	desc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);	
	desc.RasterizerState.CullMode = g_Editor.render.wireframe ? D3D12_CULL_MODE_NONE : D3D12_CULL_MODE_BACK;
	desc.RasterizerState.FillMode = g_Editor.render.wireframe ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;	
	ComPtr<ID3D12PipelineState> pso;
	CHECK_HR(device->GetNativeDevice()->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)));
	PSO = std::make_unique<PipelineState>(device, std::move(pso));	
	// indirect command buffer
	IndirectCmdSig = std::make_unique<CommandSignature>(
		device,
		*g_RHI.rootSig,
		CommandSignatureDesc(sizeof(IndirectCommand))
		.AddConstant(0, 0, 2) // b0 space0 : Mesh Index,LOD Index
		.AddVertexBufferView(0)
		.AddIndexBufferView()
		.AddDrawIndexed()
	);
}
RenderGraphPass& GBufferPass::insert(RenderGraph& rg, SceneView* sceneView, Handles const& handles){
	return rg.add_pass(L"GBuffer Generation")
		.read(*handles.command_uav.first)
		.read(*handles.depth_dsv.first)
		.read(*handles.tangentframe_rtv.first).read(*handles.gradient_rtv.first).read(*handles.material_rtv.first) // Ensures clear finishes before this
		.write(*handles.depth_dsv.first)
		.write(*handles.tangentframe_rtv.first).write(*handles.gradient_rtv.first).write(*handles.material_rtv.first)
		.execute([=](RgContext& ctx) {
			UINT width = g_Editor.render.width, height = g_Editor.render.height;
			CD3DX12_VIEWPORT viewport(.0f, .0f, width, height, .0f, 1.0f);
			CD3DX12_RECT scissorRect(0, 0, width, height);

			auto native = ctx.cmd->GetNativeCommandList();
			native->SetPipelineState(*PSO);
			native->SetGraphicsRootSignature(*g_RHI.rootSig);
			native->RSSetViewports(1, &viewport);
			native->RSSetScissorRects(1, &scissorRect);
			native->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
			native->SetGraphicsRootConstantBufferView(RHIContext::ROOTSIG_CB_EDITOR_GLOBAL, sceneView->get_editor_globals_buffer()->GetGPUAddress());

			D3D12_CPU_DESCRIPTOR_HANDLE rtvHandles[3] = {
				ctx.graph->get<RenderTargetView>(*handles.tangentframe_rtv.first)->descriptor,
				ctx.graph->get<RenderTargetView>(*handles.gradient_rtv.first)->descriptor,
				ctx.graph->get<RenderTargetView>(*handles.material_rtv.first)->descriptor
			};
			auto* r_dsv = ctx.graph->get<DepthStencilView>(*handles.depth_dsv.second);
			auto* r_indirect_commands = ctx.graph->get<Buffer>(*handles.command_uav.first);
			auto* r_indirect_commands_uav = ctx.graph->get<UnorderedAccessView>(*handles.command_uav.second);
			ctx.cmd->QueueTransitionBarrier(r_indirect_commands, ResourceState::IndirectArgument);
			ctx.cmd->FlushBarriers();

			native->OMSetRenderTargets(
				3,
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
		});
}