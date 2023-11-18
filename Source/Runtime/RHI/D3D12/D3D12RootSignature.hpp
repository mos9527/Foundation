#pragma once
#pragma once
#include "D3D12Types.hpp"
namespace RHI {
	class Device;
	class Blob;
	struct DescriptorTable {
		auto& AddSRVRange(
			UINT						 BaseShaderRegister,
			UINT						 RegisterSpace,
			UINT						 NumDescriptors,
			D3D12_DESCRIPTOR_RANGE_FLAGS Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE,
			UINT						 OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND) {
			CD3DX12_DESCRIPTOR_RANGE1& Range = ranges.emplace_back();
			Range.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, NumDescriptors, BaseShaderRegister, RegisterSpace, Flags);
			return *this;
		}

		auto& AddUAVRange(
			UINT						 BaseShaderRegister,
			UINT						 RegisterSpace,
			UINT						 NumDescriptors,
			D3D12_DESCRIPTOR_RANGE_FLAGS Flags = D3D12_DESCRIPTOR_RANGE_FLAG_DATA_VOLATILE,
			UINT						 OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND) {
			CD3DX12_DESCRIPTOR_RANGE1& Range = ranges.emplace_back();
			Range.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, NumDescriptors, BaseShaderRegister, RegisterSpace, Flags);
			return *this;
		}

		auto& AddCBVRange(
			UINT						 BaseShaderRegister,
			UINT						 RegisterSpace,
			UINT						 NumDescriptors,
			D3D12_DESCRIPTOR_RANGE_FLAGS Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE,
			UINT						 OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND) {
			CD3DX12_DESCRIPTOR_RANGE1& Range = ranges.emplace_back();
			Range.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, NumDescriptors, BaseShaderRegister, RegisterSpace, Flags);
			return *this;
		}

		auto& AddSamplerRange(
			UINT						 BaseShaderRegister,
			UINT						 RegisterSpace,
			UINT						 NumDescriptors,
			D3D12_DESCRIPTOR_RANGE_FLAGS Flags = D3D12_DESCRIPTOR_RANGE_FLAG_NONE,
			UINT						 OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND) {
			CD3DX12_DESCRIPTOR_RANGE1& Range = ranges.emplace_back();
			Range.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, NumDescriptors, BaseShaderRegister, RegisterSpace, Flags);
			return *this;
		}

		uint GetNumRanges() { return ranges.size(); }
		const CD3DX12_DESCRIPTOR_RANGE1* GetRanges() { return ranges.data(); }
	private:
		std::vector<CD3DX12_DESCRIPTOR_RANGE1> ranges;
	};
	struct RootSignatureDesc {
		auto& AddConstant(UINT shaderRegister,
			UINT registerSpace,
			UINT num32BitValues)
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

		auto& AddDescriptorTable(
			DescriptorTable& table
		) {
			CD3DX12_ROOT_PARAMETER1 Parameter = {}; // HACK : placeholder parameter. see comment below...
			descriptorTables.push_back({ parameters.size(), table});  
			return add(Parameter); // descriptor tables are realized deferred. since its memory address may change due to reallocation			
		}

		// Different from AddUnorderedAccessView, Counted resources
		// are mapped with Descriptor Tables. This function creates one and adds it to the list
		auto& AddUnorderedAccessViewWithCounter(
			UINT shaderRegister,
			UINT registerSpace = 0,
			D3D12_ROOT_DESCRIPTOR_FLAGS flags = D3D12_ROOT_DESCRIPTOR_FLAG_NONE
		) {
			DescriptorTable table;
			table.AddCBVRange(shaderRegister, registerSpace, 1);
			AddDescriptorTable(table);
			return *this;
		}

		auto& AddStaticSampler(
			UINT shaderRegister,
			UINT registerSpace,
			SamplerDesc desc = SamplerDesc::GetTextureSamplerDesc(8),
			D3D12_STATIC_BORDER_COLOR borderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE
		)
		{
			CD3DX12_STATIC_SAMPLER_DESC& Desc = staticSamplers.emplace_back();
			Desc.Init(
				shaderRegister,
				desc.desc.Filter, 
				desc.desc.AddressU,
				desc.desc.AddressV, 
				desc.desc.AddressW, 
				desc.desc.MipLODBias, 
				desc.desc.MaxAnisotropy, 
				desc.desc.ComparisonFunc,
				borderColor,
				desc.desc.MinLOD, 
				desc.desc.MaxLOD,
				D3D12_SHADER_VISIBILITY_ALL,
				registerSpace
			);
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

		operator D3D12_ROOT_SIGNATURE_DESC1() {
			for (auto& [index, table] : descriptorTables) {
				// deferred realize descriptor tables
				CD3DX12_ROOT_PARAMETER1 Parameter = {};
				Parameter.InitAsDescriptorTable(table.GetNumRanges(), table.GetRanges());
				parameters[index] = Parameter;
			}
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
		std::vector<std::pair<uint,DescriptorTable>> descriptorTables;
	};
	class RootSignature : public DeviceChild {
		name_t m_Name;
		ComPtr<ID3D12RootSignature> m_RootSignature;

	public:
		/* pulls root signature from a shader blob */
		RootSignature(Device* device, Blob* blob);
		/* build from locally created root signature*/
		RootSignature(Device* device, RootSignatureDesc& desc);

		inline auto GetNativeRootSignature() { return m_RootSignature.Get(); }
		inline operator ID3D12RootSignature* () { return m_RootSignature.Get(); }

		inline auto const& GetName() { return m_Name; }
		inline void SetName(name_t name) {
			m_Name = name;
			m_RootSignature->SetName(name);
		}
	};
}