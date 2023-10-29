#pragma once
#include "../../../pch.hpp"
#include "D3D12Resource.hpp"
namespace RHI {
	class Device;
	class CommandSignature {
	public:
		enum IndirectArgumentType {			
			DARW = 0,
			DISPATCH = 1,
			DISPATCH_MESH = 2,
			NUM_TYPES = 3
		};
		CommandSignature(Device* device, IndirectArgumentType indirectType);
		IndirectArgumentType const& GetType() { return m_Type; }
		auto GetNativeSignature() { return m_Signature.Get(); }
		inline operator ID3D12CommandSignature* () { return m_Signature.Get(); }
	private:
		ComPtr<ID3D12CommandSignature> m_Signature;
		IndirectArgumentType m_Type;
	};
}