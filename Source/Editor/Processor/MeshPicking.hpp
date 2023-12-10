#pragma once
#include "Processor.hpp"
#include "ProcessPass/AreaReduce.hpp"
#include "../../Runtime/Asset/ResourceContainer.hpp"
class MeshPicking {
public:
	MeshPicking(RHI::Device* device);
	std::vector<uint> const & GetSelectedMaterialBufferAndRect(RHI::Texture* texture, RHI::ShaderResourceView* resourceSRV, uint2 point, uint2 extent);
private:
	std::vector<uint> instanceSelected;

	std::unique_ptr<RHI::Buffer> instanceSelectionMask, readbackInstanceSelectionMask;
	std::unique_ptr<RHI::UnorderedAccessView> instanceSelectionMaskUAV;

	RHI::Device* const device;

	DefaultTaskThreadPool taskpool;
	RenderGraphResourceCache cache;

	AreaReducePass proc_reduce;
};
