#pragma once
#include "../Common.hpp"
#include "D3D12Types.hpp"
namespace RHI {
	class Device;
	class CommandSignature : public RHIObject {
	public:

		CommandSignature(Device* device, IndirectArgumentType indirectType);
		IndirectArgumentType const& GetType() { return m_Type; }
		auto GetNativeSignature() { return m_Signature.Get(); }
		inline operator ID3D12CommandSignature* () { return m_Signature.Get(); }
		using RHIObject::GetName;
		inline void SetName(name_t name) {
			m_Name = name;
			m_Signature->SetName((const wchar_t*)name.c_str());
		}
	private:
		using RHIObject::m_Name;
		ComPtr<ID3D12CommandSignature> m_Signature;
		IndirectArgumentType m_Type;
	};
}