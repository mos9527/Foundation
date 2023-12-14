#include "Clear.hpp"

RenderGraphPass& ClearPass::insert_rtv(RenderGraph& rg, SceneView* sceneView, std::vector<std::pair<RgHandle*, RgHandle*>> const& texture_rtv) {
	RenderGraphPass& pass = rg.add_pass(L"Clear RenderTargets");
	for (auto texture : texture_rtv)
		pass.write(*texture.first);
	pass.execute([=](RgContext& ctx) -> void {
		auto native = ctx.cmd->GetNativeCommandList();
		for (uint i = 0; i < texture_rtv.size(); i++) {
			auto* r_texture = ctx.graph->get<RHI::Texture>(*texture_rtv[i].first);
			auto* r_rtv = ctx.graph->get<RHI::RenderTargetView>(*texture_rtv[i].second);
			CD3DX12_RECT scissorRect(0, 0, r_texture->GetDesc().width, r_texture->GetDesc().height);
			native->ClearRenderTargetView(r_rtv->get_descriptor().get_cpu_handle(), r_texture->GetClearValue().color, 1, &scissorRect);
		}
		});
	return pass;
}

RenderGraphPass& ClearPass::insert_dsv(RenderGraph& rg, SceneView* sceneView, std::vector<std::pair<RgHandle*, RgHandle*>> const& texture_dsv) {
	RenderGraphPass& pass = rg.add_pass(L"Clear Depth-Stencil");
	for (auto texture : texture_dsv)
		pass.write(*texture.first);
	pass.execute([=](RgContext& ctx) -> void {
		auto native = ctx.cmd->GetNativeCommandList();
		for (uint i = 0; i < texture_dsv.size(); i++) {
			auto* r_texture = ctx.graph->get<RHI::Texture>(*texture_dsv[i].first);
			auto* r_dsv = ctx.graph->get<RHI::DepthStencilView>(*texture_dsv[i].second);
			CD3DX12_RECT scissorRect(0, 0, r_texture->GetDesc().width, r_texture->GetDesc().height);
			native->ClearDepthStencilView(r_dsv->get_descriptor().get_cpu_handle(), D3D12_CLEAR_FLAG_DEPTH, r_texture->GetClearValue().depthStencil.depth, r_texture->GetClearValue().depthStencil.stencil, 1, &scissorRect);
		}
		});
	return pass;
}
