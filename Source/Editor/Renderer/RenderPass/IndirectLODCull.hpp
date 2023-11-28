#pragma once
#include "../Renderer.hpp"
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

	void insert_execute(RenderGraphPass& pass, SceneView* sceneView, auto&& handles);
public:
	struct IndirectLODClearPassHandles {
		RgHandle& visibilityBuffer;
	};
	struct IndirectLODEarlyCullPassHandles {
		RgHandle& visibilityBuffer;

		RgHandle& indirectCmdBuffer; // see GetCountedIndirectCmdBufferDesc
		RgHandle& indirectCmdBufferUAV; // see GetCountedIndirectCmdBufferUAVDesc		
	};
	struct IndirectLODLateCullPassHandles {
		RgHandle& visibilityBuffer;

		RgHandle& indirectCmdBuffer; // see GetCountedIndirectCmdBufferDesc
		RgHandle& indirectCmdBufferUAV; // see GetCountedIndirectCmdBufferUAVDesc

		RgHandle& transparencyIndirectCmdBuffer;
		RgHandle& transparencyIndirectCmdBufferUAV;

		RgHandle& hizTexture;
		RgHandle& hizSRV; // must covers all mips
	};
	static const RHI::Resource::ResourceDesc GetCountedIndirectCmdBufferDesc(RHI::name_t name) {
		return RHI::Resource::ResourceDesc::GetGenericBufferDesc(
			CommandBufferSize, sizeof(IndirectCommand), RHI::ResourceState::CopyDest, RHI::ResourceHeapType::Default, RHI::ResourceFlags::UnorderedAccess, name
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
	RenderGraphPass& insert_clear(RenderGraph& rg, SceneView* sceneView, IndirectLODClearPassHandles&& handles);
	RenderGraphPass& insert_earlycull(RenderGraph& rg, SceneView* sceneView, IndirectLODEarlyCullPassHandles&& handles);
	RenderGraphPass& insert_latecull(RenderGraph& rg, SceneView* sceneView, IndirectLODLateCullPassHandles&& handles);
};