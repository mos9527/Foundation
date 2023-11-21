#pragma once
#include "RenderPass.hpp"

class IndirectLODCullPass {
	static const UINT CommandSizePerFrame = MAX_INSTANCE_COUNT * sizeof(IndirectCommand);
	static const UINT CommandBufferCounterOffset = AlignForUavCounter(CommandSizePerFrame);
	static const UINT CommandBufferSize = CommandBufferCounterOffset + sizeof(UINT); // including a UAV Counter	

	std::unique_ptr<RHI::Shader> cullPassCS;
	std::unique_ptr<RHI::RootSignature> cullPassRS;
	std::unique_ptr<RHI::PipelineState> cullPassPSO;

	std::unique_ptr<RHI::Buffer> resetBuffer; // a buffer that can be used to reset the UAV counters and initialize it 0
public:
	struct IndirectLODCullPassHandles {
		RgHandle& indirectCmdBuffer; // see GetCountedIndirectCmdBufferDesc
		RgHandle& indirectCmdBufferUAV; // see GetCountedIndirectCmdBufferUAVDesc
		// xxx reading culled mesh indices from command buffer seems like a hack...is it bad though?
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
	void insert(RenderGraph& rg, SceneGraphView* sceneView, IndirectLODCullPassHandles& handles);
};