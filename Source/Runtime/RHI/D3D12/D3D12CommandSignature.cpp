#include "D3D12CommandSignature.hpp"
#include "D3D12Device.hpp"
#include "D3D12RootSignature.hpp"
namespace RHI {
	CommandSignature::CommandSignature(Device* device, RootSignature* rootSig, CommandSignatureDesc& desc) : DeviceChild(device) {
		D3D12_COMMAND_SIGNATURE_DESC cdesc = desc;
		CHECK_HR(device->GetNativeDevice()->CreateCommandSignature(&cdesc, rootSig ? *rootSig : NULL, IID_PPV_ARGS(&m_Signature)));
	}
}