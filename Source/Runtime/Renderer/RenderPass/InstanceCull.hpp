#pragma once
#include "../Renderer.hpp"
struct InstanceCull : public IRenderPass {	
	static const UINT CommandSizePerFrame = MAX_INSTANCE_COUNT * sizeof(IndirectCommand);
	static const UINT CommandBufferCounterOffset = AlignForUavCounter(CommandSizePerFrame);
	static const UINT CommandBufferSize = CommandBufferCounterOffset + sizeof(UINT); // including a UAV Counter	
	
	std::unique_ptr<RHI::Shader> CS_Early, CS_Late;
	std::unique_ptr<RHI::PipelineState> PSO_Early, PSO_Late;

	std::pair<std::unique_ptr<RHI::Buffer>, std::unique_ptr<RHI::UnorderedAccessView>> visibility;
	std::unique_ptr<BufferContainer<InstanceCullConstant>> constants;
public:
	struct Handles {
		std::pair<RgHandle*, RgHandle*> hiz_srv;
		std::array<std::tuple<RgHandle*, RgHandle*, int, int, bool>, INSTANCE_CULL_MAX_CMDS> cmd_uav_instanceMaskAllow_instanceMaskRejcectClearCounter;
	};	
	static const RHI::Resource::ResourceDesc GetCountedIndirectCmdBufferDesc(RHI::name_t name) {
		return RHI::Resource::ResourceDesc::GetGenericBufferDesc(
			CommandBufferSize, sizeof(IndirectCommand), RHI::ResourceState::CopyDest, RHI::ResourceHeapType::Default, RHI::ResourceFlags::UnorderedAccess, name
		);
	}
	static const RgUAV::view_desc GetCountedIndirectCmdBufferUAVDesc(RgHandle const& indirectBuffer) {
		return RgUAV::view_desc{
			 .viewDesc = RHI::UnorderedAccessViewDesc::GetStructuredBufferDesc(0, MAX_INSTANCE_COUNT, sizeof(IndirectCommand), CommandBufferCounterOffset),
			 .viewed = indirectBuffer,
			 .viewedCounter = indirectBuffer
		};
	}
	InstanceCull(RHI::Device* device) : IRenderPass(device, { L"InstanceCull" }, "Instance Culling") { reset(); };
	
	virtual void reset();
	RenderGraphPass& insert(RenderGraph& rg, SceneView* sceneView, Handles const& handles, bool late = false);	
};
