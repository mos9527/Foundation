#pragma once
#include "RenderPass.hpp"

class IndirectLODCullPass {
	static const UINT CommandSizePerFrame = MAX_INSTANCE_COUNT * sizeof(IndirectCommand);
	static const UINT CommandBufferCounterOffset = AlignForUavCounter(CommandSizePerFrame);
	static const UINT CommandBufferSize = CommandBufferCounterOffset + sizeof(UINT); // including a UAV Counter	

	std::unique_ptr<RHI::Shader> depthSampleCS;
	std::unique_ptr<RHI::RootSignature> depthSampleRS;
	std::unique_ptr<RHI::PipelineState> depthSamplePSO;

	std::unique_ptr<RHI::Shader> cullPassEarlyCS, cullPassLateCS;
	std::unique_ptr<RHI::RootSignature> cullPassRS;
	std::unique_ptr<RHI::PipelineState> cullPassEarlyPSO, cullPassLatePSO;

	void insert_execute(RenderGraphPass& pass, SceneGraphView* sceneView, auto& handles);
public:
	struct IndirectLODEarlyCullPassHandles {
		RgHandle& visibilityBuffer;

		RgHandle& indirectCmdBuffer; // see GetCountedIndirectCmdBufferDesc
		RgHandle& indirectCmdBufferUAV; // see GetCountedIndirectCmdBufferUAVDesc		
	};
	struct IndirectLODLateCullPassHandles {
		RgHandle& visibilityBuffer;

		RgHandle& indirectCmdBuffer; // see GetCountedIndirectCmdBufferDesc
		RgHandle& indirectCmdBufferUAV; // see GetCountedIndirectCmdBufferUAVDesc

		RgHandle& hizTexture;
		RgHandle& hizSRV; // must covers all mips
	};
	static const RHI::Resource::ResourceDesc GetCountedIndirectCmdBufferDesc() {
		return RHI::Resource::ResourceDesc::GetGenericBufferDesc(
			CommandBufferSize, sizeof(IndirectCommand), RHI::ResourceState::CopyDest, RHI::ResourceHeapType::Default, RHI::ResourceFlags::UnorderedAccess, L"Indirect CMD Buffer"
		);
	}
	static const RgUAV::view_desc GetCountedIndirectCmdBufferUAVDesc(RgHandle indirectBuffer) {
		return RgUAV::view_desc{
			 .viewDesc = RHI::UnorderedAccessViewDesc::GetStructuredBufferDesc(0, MAX_INSTANCE_COUNT, sizeof(IndirectCommand), CommandBufferCounterOffset),
			 .viewed = indirectBuffer,
			 .viewedCounter = indirectBuffer
		};
	}
	IndirectLODCullPass(RHI::Device* device);
	RenderGraphPass& insert_earlycull(RenderGraph& rg, SceneGraphView* sceneView, IndirectLODEarlyCullPassHandles& handles);
	RenderGraphPass& insert_latecull(RenderGraph& rg, SceneGraphView* sceneView, IndirectLODLateCullPassHandles& handles);
};