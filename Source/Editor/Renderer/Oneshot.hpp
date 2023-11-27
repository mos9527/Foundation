#pragma once
#include "Renderer.hpp"
#include "RenderPass/IBLPrefilter.hpp"

template<typename PassType> class OneshotPass {
public:
	OneshotPass(RHI::Device* device) {};
};
// Specializaton for passes
template<> class OneshotPass<IBLPrefilterPass> {	
public:
	OneshotPass(RHI::Device* device) :
		device(device), proc_Prefilter(device)
	{};
	void Process(TextureAsset* srcImage);
private:
	RHI::Device* const device;
	RenderGraphResourceCache cache;

	std::unique_ptr<RHI::Texture> cubeMap;
	std::vector<std::unique_ptr<RHI::UnorderedAccessView>> cubeMapUAV;

	IBLPrefilterPass proc_Prefilter;
};