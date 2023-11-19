#pragma once
#include "RenderPass.hpp"

class GBufferPass {
	const UINT CommandSizePerFrame = MAX_INSTANCE_COUNT * sizeof(IndirectCommand);
	const UINT CommandBufferCounterOffset = AlignForUavCounter(CommandSizePerFrame);
	const UINT CommandBufferSize = CommandBufferCounterOffset + sizeof(UINT); // including a UAV Counter	

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
public:
	struct GBufferPassHandles {
		RgHandle& depth;
				
		RgHandle& albedo;
		RgHandle& normal;
		RgHandle& material;
		RgHandle& emissive;
				
		RgHandle& depth_dsv;
				
		RgHandle& albedo_rtv;
		RgHandle& normal_rtv;
		RgHandle& material_rtv;
		RgHandle& emissive_rtv;
	};
	GBufferPass(RHI::Device* device);
	void insert(RenderGraph& rg, SceneGraphView* sceneView, GBufferPassHandles& handles);
};