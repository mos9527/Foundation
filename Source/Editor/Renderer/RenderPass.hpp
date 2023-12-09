#pragma once
#include "../../Runtime/RHI/RHI.hpp"
struct IRenderPass {
	const char* const name;
	const char* const desc;	
	RHI::Device* const device;

	IRenderPass(RHI::Device* device, const char* name = "<to be filled>", const char* desc = "<to be filled>") : device(device), name(name), desc(desc) {};
	virtual void setup() = 0;
};
