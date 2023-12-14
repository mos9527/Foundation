#pragma once
#include "AssetComponent.hpp"
#include "../../Runtime/RHI/RHI.hpp"
class HDRIProbeProcessor;
struct AssetHDRIProbeComponent : public AssetComponent {
	static const AssetComponentType type = AssetComponentType::HDRIProbe;
	HDRIProbeProcessor* probe{};
	AssetHDRIProbeComponent(Scene& parent, entt::entity entity) : AssetComponent(parent, entity, type) {};
	~AssetHDRIProbeComponent();
	void setup(RHI::Device* device, uint dimension);
	void load(AssetHandle hdriImage_);
private:
	AssetHandle hdriImage;
};