#pragma once
#include "Renderer.hpp"
class DeferredRenderer : public Renderer {
public:
	DeferredRenderer(AssetRegistry& assets, SceneGraph& scene, RHI::Device* device, RHI::Swapchain* swapchain);
	virtual void Render();
private:
	void ResetCounter(RHI::CommandList* cmd,RHI::Resource* resource, size_t counterOffset);
	struct IndirectCommand {
		D3D12_VERTEX_BUFFER_VIEW	 VertexBuffer;
		D3D12_INDEX_BUFFER_VIEW		 IndexBuffer;
		D3D12_DRAW_INDEXED_ARGUMENTS DrawIndexedArguments;
	};
	const UINT CommandSizePerFrame = MAX_INSTANCE_COUNT * sizeof(IndirectCommand);
	const UINT CommandBufferCounterOffset = AlignForUavCounter(CommandSizePerFrame);
	const UINT CommandBufferSize = CommandBufferCounterOffset + sizeof(UINT); // including a UAV Counter	

	std::unique_ptr<SceneGraphView> sceneView;

	std::unique_ptr<RHI::Shader> cullPassCS;
	std::unique_ptr<RHI::RootSignature> cullPassRS;
	std::unique_ptr<RHI::PipelineState> cullPassPSO;

	std::unique_ptr<RHI::Shader> gBufferVS, gBufferPS;
	std::unique_ptr<RHI::RootSignature> gBufferRS;
	std::unique_ptr<RHI::PipelineState> gBufferPSO;
	std::unique_ptr<RHI::CommandSignature> gBufferIndirectCommandSig;

	std::unique_ptr<RHI::Buffer> resetBuffer; // a buffer that can be used to reset the UAV counters and initialize it 0
	std::unique_ptr<RHI::Buffer> indirectCmdBuffer;
	std::unique_ptr<RHI::UnorderedAccessView> indirectCmdBufferUAV;
	RenderGraphResourceCache cache;
};