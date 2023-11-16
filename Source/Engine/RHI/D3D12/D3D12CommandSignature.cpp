#include "D3D12CommandSignature.hpp"
#include "D3D12Device.hpp"
namespace RHI {

	CommandSignature::CommandSignature(Device* device, IndirectArgumentType indirectType) : DeviceChild(device), m_Type(indirectType) {
		D3D12_COMMAND_SIGNATURE_DESC desc{};
		D3D12_INDIRECT_ARGUMENT_DESC argument_desc{};
		desc.NumArgumentDescs = 1;
		argument_desc.Type = IndirectArgumentToD3DType(indirectType);
		desc.pArgumentDescs = &argument_desc;
		desc.ByteStride = IndirectArgumentToSize(indirectType);
		CHECK_HR(device->GetNativeDevice()->CreateCommandSignature(&desc, nullptr, IID_PPV_ARGS(&m_Signature)));
	}
}