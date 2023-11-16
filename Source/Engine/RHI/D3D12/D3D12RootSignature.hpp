#pragma once
#pragma once
#include "D3D12Types.hpp"
namespace RHI {
	class Device;
	class ShaderBlob;
	class RootSignature : public DeviceChild {
		name_t m_Name;
		ComPtr<ID3D12RootSignature> m_RootSignature;

	public:
		/* pulls root signature from a shader blob */
		RootSignature(Device* device, ShaderBlob* blob);
		inline auto GetNativeRootSignature() { return m_RootSignature.Get(); }
		inline operator ID3D12RootSignature* () { return m_RootSignature.Get(); }

		inline auto const& GetName() { return m_Name; }
		inline void SetName(name_t name) {
			m_Name = name;
			m_RootSignature->SetName((const wchar_t*)name.c_str());			
		}
	};
}