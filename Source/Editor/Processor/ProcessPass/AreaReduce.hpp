#pragma once
#include "../Processor.hpp"

class AreaReducePass {
	std::unique_ptr<RHI::Shader> CS;	
	std::unique_ptr<RHI::PipelineState> PSO;	

	std::unique_ptr<BufferContainer<AreaReduceConstant>> constants;
public:
	struct AreaReducePassHandles {
		std::pair<RgHandle*, RgHandle*> material_srv;
		std::pair<RgHandle*, RgHandle*> selection_uav;		
	};
	AreaReducePass(RHI::Device*);
	RenderGraphPass& insert_clear(RenderGraph& rg, AreaReducePassHandles const& handles);
	RenderGraphPass& insert_reduce_material_instance(RenderGraph& rg, AreaReducePassHandles const& handles, uint2 point, uint2 extent);
};