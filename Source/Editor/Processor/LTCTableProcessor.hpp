#pragma once
#include "Processor.hpp"
class LTCTableProcessor {
public:
	std::unique_ptr<RHI::Texture> ltcLUT;
	std::unique_ptr<RHI::ShaderResourceView> ltcSRV;
	LTCTableProcessor(RHI::Device* device);	
private:
	void UploadPrecomputed();

	RHI::Device* const device;

	DefaultTaskThreadPool taskpool;	
};
