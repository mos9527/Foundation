#pragma once
#include "AssetComponent.hpp"
#include "../../Runtime/RHI/RHI.hpp"
class HDRIProbeProcessor;
struct AssetHDRIProbeComponent : public AssetComponent {
	static const AssetComponentType type = AssetComponentType::HDRIProbe;
	AssetHDRIProbeComponent(Scene& parent, entt::entity entity) : AssetComponent(parent, entity, type) {};
	~AssetHDRIProbeComponent();
	void setup(RHI::Device* device, uint dimension);
	void load(AssetHandle hdriImage_);
	inline HDRIProbeProcessor* get_probe() {
		CHECK(probe && "Probe did not initialize. Please call setup()");
		return probe;
	}
private:
	HDRIProbeProcessor* probe{};
	AssetHandle hdriImage;
};