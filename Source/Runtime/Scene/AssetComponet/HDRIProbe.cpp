#include "HDRIProbe.hpp"
#include "../Processor/HDRIProbe.hpp"
void AssetHDRIProbeComponent::setup(RHI::Device* device, uint dimension) {
	probe = new ::HDRIProbeProcessor(device, dimension);
};
AssetHDRIProbeComponent::~AssetHDRIProbeComponent() {
	delete probe;
}
void AssetHDRIProbeComponent::load(AssetHandle hdriImage_) {
	CHECK(probe);
	hdriImage = hdriImage_;
	auto& image = parent.get<TextureAsset>(hdriImage);
	probe->ProcessAsync(&image);
}
