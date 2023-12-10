#pragma once
#include "Processor.hpp"
class LTCFittedLUT {
public:
	std::unique_ptr<RHI::Texture> ltcLUT;
	std::unique_ptr<RHI::ShaderResourceView> ltcSRV;
	LTCFittedLUT(RHI::Device* device);	
private:
	void UploadPrecomputed();

	RHI::Device* const device;

	DefaultTaskThreadPool taskpool;	
};
