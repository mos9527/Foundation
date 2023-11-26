#pragma once
#include "Renderer.hpp"
#include "RenderPass/IBLPrefilter.hpp"

class OneshotPass {
public:
	OneshotPass(RHI::Device* device) :
		device(device), proc_Prefilter(device)
	{};
	void IBLPrefilterProcess();
private:
	RHI::Device* const device;
	RenderGraphResourceCache cache;

	IBLPrefilterPass proc_Prefilter;
};