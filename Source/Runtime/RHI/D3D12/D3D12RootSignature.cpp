#include "D3D12RootSignature.hpp"
#include "D3D12Device.hpp"
#include "D3D12Shader.hpp"
namespace RHI {
	RootSignature::RootSignature(Device* device, Blob* blob) : DeviceChild(device) {
		CHECK_HR(device->GetNativeDevice()->CreateRootSignature(
			0, blob->GetData(), blob->GetSize(), IID_PPV_ARGS(&m_RootSignature)
		));
	}
	RootSignature::RootSignature(Device* device, RootSignatureDesc const& desc) : DeviceChild(device) {
		D3D12_ROOT_SIGNATURE_DESC1 cdesc = desc;
		D3D12_VERSIONED_ROOT_SIGNATURE_DESC versionedDesc = {
			.Version  = D3D_ROOT_SIGNATURE_VERSION_1_1,
			.Desc_1_1 = cdesc
		};
		ComPtr<ID3DBlob> blob;
		CHECK_HR(D3D12SerializeVersionedRootSignature(&versionedDesc, &blob, nullptr));
		CHECK_HR(device->GetNativeDevice()->CreateRootSignature(
			0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&m_RootSignature)
		));
	}
}