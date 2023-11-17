#pragma once
#pragma once
#include "D3D12Types.hpp"
namespace RHI {
	class Device;
	class Blob;
	struct RootSignatureDesc {
		auto& Add32BitConstants(UINT num32BitValues,
			UINT shaderRegister,
			UINT registerSpace = 0)
		{
			CD3DX12_ROOT_PARAMETER1 Parameter = {};
			Parameter.InitAsConstants(num32BitValues, shaderRegister, registerSpace);
			return add(Parameter);
		}

		auto& AddConstantBufferView(
			UINT shaderRegister,
			UINT registerSpace = 0,
			D3D12_ROOT_DESCRIPTOR_FLAGS flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE
		) {
			CD3DX12_ROOT_PARAMETER1 Parameter = {};
			Parameter.InitAsConstantBufferView(shaderRegister, registerSpace, flags);
			return add(Parameter);
		}


		auto& AddShaderResourceView(
			UINT shaderRegister,
			UINT registerSpace = 0,
			D3D12_ROOT_DESCRIPTOR_FLAGS flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE
		) {
			CD3DX12_ROOT_PARAMETER1 Parameter = {};
			Parameter.InitAsShaderResourceView(shaderRegister, registerSpace, flags);
			return add(Parameter);
		}

		auto& AddUnorderedAccessView(
			UINT shaderRegister,
			UINT registerSpace = 0,
			D3D12_ROOT_DESCRIPTOR_FLAGS flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE
		) {
			CD3DX12_ROOT_PARAMETER1 Parameter = {};
			Parameter.InitAsUnorderedAccessView(shaderRegister, registerSpace, flags);
			return add(Parameter);
		}
		// Different from AddUnorderedAccessView, Counted resources
		// are mapped with Descriptor Tables
		auto& AddUnorderedAccessViewWithCounter(
			UINT shaderRegister,
			UINT registerSpace = 0,
			D3D12_ROOT_DESCRIPTOR_FLAGS flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE
		) {
			CD3DX12_DESCRIPTOR_RANGE1 Range;
			Range.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE);
			CD3DX12_ROOT_PARAMETER1 Parameter = {};
			Parameter.InitAsDescriptorTable(1, &Range);
			return add(Parameter);
		}

		auto& AddSampler(
			UINT shaderRegister,
			D3D12_FILTER filter = D3D12_FILTER_ANISOTROPIC,
			D3D12_TEXTURE_ADDRESS_MODE addressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
			D3D12_TEXTURE_ADDRESS_MODE addressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
			D3D12_TEXTURE_ADDRESS_MODE addressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP,
			FLOAT mipLODBias = 0,
			UINT maxAnisotropy = 16,
			D3D12_COMPARISON_FUNC comparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL,
			D3D12_STATIC_BORDER_COLOR borderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE,
			FLOAT minLOD = 0.f,
			FLOAT maxLOD = D3D12_FLOAT32_MAX
		)
		{
			CD3DX12_STATIC_SAMPLER_DESC& Desc = staticSamplers.emplace_back();
			Desc.Init(shaderRegister, filter, addressU, addressV, addressW, mipLODBias, maxAnisotropy, comparisonFunc, borderColor, minLOD, maxLOD);			
			return *this;
		}

		auto& SetDirectlyIndexed(){
			flags |= D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;
			flags |= D3D12_ROOT_SIGNATURE_FLAG_SAMPLER_HEAP_DIRECTLY_INDEXED;
			return *this;
		}

		auto& SetAllowInputAssembler() {
			flags |= D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
			return *this;
		}

		operator D3D12_ROOT_SIGNATURE_DESC1() const {
			return D3D12_ROOT_SIGNATURE_DESC1{
				.NumParameters = (UINT)parameters.size(),
				.pParameters = parameters.data(),
				.NumStaticSamplers = (UINT)staticSamplers.size(),
				.pStaticSamplers = staticSamplers.data(),
				.Flags = flags
			};			
		}
	private:
		RootSignatureDesc& add(D3D12_ROOT_PARAMETER1& param) {
			parameters.emplace_back(param);
			return *this;
		}
		D3D12_ROOT_SIGNATURE_FLAGS flags{};
		std::vector<CD3DX12_ROOT_PARAMETER1> parameters;
		std::vector<CD3DX12_STATIC_SAMPLER_DESC> staticSamplers;
	};
	class RootSignature : public DeviceChild {
		name_t m_Name;
		ComPtr<ID3D12RootSignature> m_RootSignature;

	public:
		/* pulls root signature from a shader blob */
		RootSignature(Device* device, Blob* blob);
		/* build from locally created root signature*/
		RootSignature(Device* device, RootSignatureDesc const& desc);

		inline auto GetNativeRootSignature() { return m_RootSignature.Get(); }
		inline operator ID3D12RootSignature* () { return m_RootSignature.Get(); }

		inline auto const& GetName() { return m_Name; }
		inline void SetName(name_t name) {
			m_Name = name;
			m_RootSignature->SetName(name);
		}
	};
}