#pragma once
#include "Renderer.hpp"
class DeferredRenderer : public Renderer {
public:
	DeferredRenderer(AssetRegistry& assets, SceneGraph& scene, RHI::Device* device, RHI::Swapchain* swapchain);
	virtual RHI::ShaderResourceView* Render();
	void SetViewportSize(uint width, uint height) {
		viewportSize.x = std::max(128u,width);
		viewportSize.y = std::max(128u,height);
	}
	uint2 GetViewportSize() { return viewportSize; }
private:
	void ResetCounter(RHI::CommandList* cmd,RHI::Resource* resource, size_t counterOffset);
	const UINT CommandSizePerFrame = MAX_INSTANCE_COUNT * sizeof(IndirectCommand);
	const UINT CommandBufferCounterOffset = AlignForUavCounter(CommandSizePerFrame);
	const UINT CommandBufferSize = CommandBufferCounterOffset + sizeof(UINT); // including a UAV Counter	
	uint2 viewportSize{ 128, 128 };

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

	std::unique_ptr<RHI::Shader> lightingCS;
	std::unique_ptr<RHI::RootSignature> lightingRS;
	std::unique_ptr<RHI::PipelineState> lightingPSO;

	RenderGraphResourceCache cache; // xxx multiple command lists?
};