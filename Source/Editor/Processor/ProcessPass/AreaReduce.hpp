#pragma once
#include "../Processor.hpp"

class AreaReducePass {
	std::unique_ptr<RHI::Shader> CS;
	std::unique_ptr<RHI::RootSignature> RS;
	std::unique_ptr<RHI::PipelineState> PSO;	
public:
	struct AreaReducePassHandles {
		RgHandle& texture;
		RgHandle& textureSRV;
		RgHandle& output;
		RgHandle& outputUAV;
	};
	AreaReducePass(RHI::Device*);
	RenderGraphPass& insert_reduce_material_instance(RenderGraph& rg, AreaReducePassHandles&& handles, uint2 point, uint2 extent);
};