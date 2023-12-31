#include "MeshPicking.hpp"
using namespace RHI;

MeshPicking::MeshPicking(Device* device) : device(device), proc_reduce(device) {
	instanceSelectionMask = std::make_unique<Buffer>(device, Resource::ResourceDesc::GetGenericBufferDesc(
		(MAX_INSTANCE_COUNT) * sizeof(UINT), RAW_BUFFER_STRIDE, ResourceState::UnorderedAccess, ResourceHeapType::Default,
		ResourceFlags::UnorderedAccess
	));
	instanceSelectionMask->SetName(L"Instance Selection Mask");
	instanceSelectionMaskUAV = std::make_unique<UnorderedAccessView>(
		instanceSelectionMask.get(),
		UnorderedAccessViewDesc::GetRawBufferDesc(0, MAX_INSTANCE_COUNT)
	);
	readbackInstanceSelectionMask = std::make_unique<Buffer>(device, Resource::ResourceDesc::GetGenericBufferDesc(
		(MAX_INSTANCE_COUNT) * sizeof(UINT), RAW_BUFFER_STRIDE, ResourceState::CopyDest, ResourceHeapType::Readback
	));
	readbackInstanceSelectionMask->SetName(L"Readback Instance Selection Mask");	
}

std::vector<uint> const & MeshPicking::GetSelectedMaterialBufferAndRect(RHI::Texture* texture, RHI::ShaderResourceView* resourceSRV, uint2 point, uint2 extent) {
	RenderGraph rg(cache);
	auto& output = rg.import<Buffer>(instanceSelectionMask.get());
	auto& outputUav = rg.import<UnorderedAccessView>(instanceSelectionMaskUAV.get());
	proc_reduce.insert_clear(rg, {
		.selection_uav = { &output, &outputUav }
	});
	proc_reduce.insert_reduce_material_instance(
		rg, {
			.material_srv = { &rg.import<Texture>(texture), &rg.import<ShaderResourceView>(resourceSRV) },
			.selection_uav = { &output, &outputUav }
		}, point, extent
	);
	auto* cmd = device->GetDefaultCommandList<CommandListType::Direct>();	
	CHECK(!cmd->IsOpen() && "The Default Compute Command list is in use!");	
	cmd->Begin();	
	static ID3D12DescriptorHeap* const heaps[] = { device->GetOnlineDescriptorHeap<DescriptorHeapType::CBV_SRV_UAV>()->GetNativeHeap() };
	cmd->GetNativeCommandList()->SetDescriptorHeaps(1, heaps);
	rg.get_epilogue_pass().read(output);
	rg.execute(cmd);
	// Copy reduced buffer to readback
	cmd->QueueTransitionBarrier(instanceSelectionMask.get(), ResourceState::CopySource);
	cmd->FlushBarriers();
	cmd->CopyBufferRegion(instanceSelectionMask.get(), readbackInstanceSelectionMask.get(), 0, 0, instanceSelectionMask->GetDesc().width);
	cmd->Close();
	cmd->Execute().Wait();	
	void* pMappedData = readbackInstanceSelectionMask->Map(0);
	instanceSelected.clear();
	for (uint i = 0; i < MAX_INSTANCE_COUNT; i++) {
		UINT selected = *reinterpret_cast<UINT*>((reinterpret_cast<BYTE*>(pMappedData) + i * 4));
		if (selected != 0xffffffff) {
			instanceSelected.push_back(i);
		}
	}
	readbackInstanceSelectionMask->Unmap(0);
	return instanceSelected;
}
