#pragma once
#include "Processor.hpp"
#include "ProcessPass/AreaReduce.hpp"
#include "../../Runtime/Asset/ResourceContainer.hpp"
class MeshSelectionProcessor {
public:
	MeshSelectionProcessor(RHI::Device* device);

	std::vector<bool>& UpdateByMaterialBufferAndRect(RHI::Texture* texture, RHI::ShaderResourceView* resourceSRV, uint2 point, uint2 extent, bool multiSelect = false);

	inline std::vector<bool>& GetSelection() { return instanceSelected; };	
	inline bool GetSelected(UINT instanceIndex) { return instanceSelected[instanceIndex]; }
	inline void SetSelected(UINT instanceIndex, bool selected) { instanceSelected[instanceIndex] = selected; }	
private:
	std::vector<bool> instanceSelected;

	std::unique_ptr<RHI::Buffer> instanceSelectionMask, readbackInstanceSelectionMask;
	std::unique_ptr<RHI::UnorderedAccessView> instanceSelectionMaskUAV;

	void* pMappedData;

	RHI::Device* const device;

	DefaultTaskThreadPool taskpool;
	RenderGraphResourceCache cache;

	AreaReducePass proc_reduce;
};
