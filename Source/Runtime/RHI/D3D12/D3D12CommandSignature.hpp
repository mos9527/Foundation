#pragma once
#include "D3D12Types.hpp"
namespace RHI {
	class Device;
	class RootSignature;
	struct CommandSignatureDesc {
		CommandSignatureDesc(uint commandStride) : stride(commandStride) {};
		auto& AddDrawIndexed() {
			D3D12_INDIRECT_ARGUMENT_DESC desc{};
			desc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
			return add(desc);
		}
		auto& AddDispatch() {
			D3D12_INDIRECT_ARGUMENT_DESC desc{};
			desc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
			return add(desc);
		}
		auto& AddVertexBufferView(UINT slot)
		{
			D3D12_INDIRECT_ARGUMENT_DESC desc{};
			desc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_VERTEX_BUFFER_VIEW;
			desc.VertexBuffer.Slot = slot;
			return add(desc);
		}
		auto& AddIndexBufferView()
		{
			D3D12_INDIRECT_ARGUMENT_DESC desc{};
			desc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_INDEX_BUFFER_VIEW;
			return add(desc);
		}
		auto& AddConstant(UINT RootParameterIndex, UINT DestOffsetIn32BitValues, UINT Num32BitValuesToSet)
		{
			D3D12_INDIRECT_ARGUMENT_DESC desc{};
			desc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT;
			desc.Constant.RootParameterIndex = RootParameterIndex;
			desc.Constant.DestOffsetIn32BitValues = DestOffsetIn32BitValues;
			desc.Constant.Num32BitValuesToSet = Num32BitValuesToSet;
			return add(desc);
		}

		auto& AddConstantBufferView(UINT RootParameterIndex)
		{
			D3D12_INDIRECT_ARGUMENT_DESC desc{};
			desc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_CONSTANT_BUFFER_VIEW;
			desc.ConstantBufferView.RootParameterIndex = RootParameterIndex;
			return add(desc);
		}

		auto& AddShaderResourceView(UINT RootParameterIndex)
		{
			D3D12_INDIRECT_ARGUMENT_DESC desc{};
			desc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_SHADER_RESOURCE_VIEW;
			desc.ShaderResourceView.RootParameterIndex = RootParameterIndex;
			return add(desc);
		}

		auto& AddUnorderedAccessView(UINT RootParameterIndex)
		{
			D3D12_INDIRECT_ARGUMENT_DESC desc{};
			desc.Type = D3D12_INDIRECT_ARGUMENT_TYPE_UNORDERED_ACCESS_VIEW;
			desc.UnorderedAccessView.RootParameterIndex = RootParameterIndex;
			return add(desc);
		}
		operator D3D12_COMMAND_SIGNATURE_DESC() {
			return D3D12_COMMAND_SIGNATURE_DESC{
				.ByteStride = stride,
				.NumArgumentDescs = (UINT)parameters.size(),
				.pArgumentDescs = parameters.data(),
				.NodeMask = 0
			};	
		}
	private:
		CommandSignatureDesc& add(D3D12_INDIRECT_ARGUMENT_DESC& desc) {
			parameters.emplace_back(desc);
			return *this;
		}
		uint stride;
		std::vector<D3D12_INDIRECT_ARGUMENT_DESC> parameters;		
	};
	class CommandSignature : public DeviceChild {
	public:	
		CommandSignature(Device* device, RootSignature* rootSig, CommandSignatureDesc& desc);

		auto GetNativeSignature() { return m_Signature.Get(); }
		inline operator ID3D12CommandSignature* () { return m_Signature.Get(); }
		inline auto const& GetName() { return m_Name; }
		inline void SetName(name_t name) {
			m_Name = name;
			m_Signature->SetName(name);
		}
	private:
		name_t m_Name;
		ComPtr<ID3D12CommandSignature> m_Signature;		
	};
}