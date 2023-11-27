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
	OneshotPass(RHI::Device* device, uint dimesnion);
	void Process(TextureAsset* srcImage);
	uint dimension, numMips;
private:
	RHI::Device* const device;
	RenderGraphResourceCache cache;

	std::unique_ptr<RHI::Texture> cubeMap, irridanceMap, radianceMapArray, lutArray;
	std::vector<std::unique_ptr<RHI::UnorderedAccessView>> cubeMapUAVs, radianceCubeArrayUAVs;
	std::unique_ptr<RHI::UnorderedAccessView> irridanceMapUAV, lutArrayUAV;
	std::unique_ptr<RHI::ShaderResourceView> cubemapSRV, irridanceCubeSRV, radianceCubeArraySRV, lutArraySRV;

	IBLPrefilterPass proc_Prefilter;
};