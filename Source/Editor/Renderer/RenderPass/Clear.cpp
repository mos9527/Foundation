#include "Clear.hpp"

RenderGraphPass& ClearPass::insert_rtv(RenderGraph& rg, SceneView* sceneView, std::vector<RgHandle*>&& textures, std::vector<RgHandle*>&& textureRTVs) {
	RenderGraphPass& pass = rg.add_pass(L"Clear RenderTargets");
	for (auto texture : textures)
		pass.write(*texture);
	pass.execute([=](RgContext& ctx) -> void {
		auto native = ctx.cmd->GetNativeCommandList();
		for (uint i = 0; i < textures.size(); i++) {
			auto* r_texture = ctx.graph->get<RHI::Texture>(*textures[i]);
			auto* r_rtv = ctx.graph->get<RHI::RenderTargetView>(*textureRTVs[i]);
			CD3DX12_RECT scissorRect(0, 0, r_texture->GetDesc().width, r_texture->GetDesc().height);
			native->ClearRenderTargetView(r_rtv->descriptor.get_cpu_handle(), r_texture->GetClearValue().color, 1, &scissorRect);
		}
		});
	return pass;
}

RenderGraphPass& ClearPass::insert_dsv(RenderGraph& rg, SceneView* sceneView, std::vector<RgHandle*>&& textures, std::vector<RgHandle*>&& textureDSVs) {
	RenderGraphPass& pass = rg.add_pass(L"Clear Depth-Stencil");
	for (auto texture : textures)
		pass.write(*texture);
	pass.execute([=](RgContext& ctx) -> void {
		auto native = ctx.cmd->GetNativeCommandList();
		for (uint i = 0; i < textures.size(); i++) {
			auto* r_texture = ctx.graph->get<RHI::Texture>(*textures[i]);
			auto* r_dsv = ctx.graph->get<RHI::DepthStencilView>(*textureDSVs[i]);
			CD3DX12_RECT scissorRect(0, 0, r_texture->GetDesc().width, r_texture->GetDesc().height);
			native->ClearDepthStencilView(r_dsv->descriptor.get_cpu_handle(), D3D12_CLEAR_FLAG_DEPTH, r_texture->GetClearValue().depthStencil.depth, r_texture->GetClearValue().depthStencil.stencil, 1, &scissorRect);
		}
		});
	return pass;
}
