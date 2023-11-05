#include "D3D12RootSignature.hpp"
#include "D3D12Device.hpp"
#include "D3D12Shader.hpp"
namespace RHI {
	RootSignature::RootSignature(Device* device, ShaderBlob* blob){
		CHECK_HR(device->GetNativeDevice()->CreateRootSignature(0, blob->GetData(), blob->GetSize(), IID_PPV_ARGS(&m_RootSignature)));
	}
}