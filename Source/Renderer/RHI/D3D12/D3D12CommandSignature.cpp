#include "D3D12CommandSignature.hpp"
#include "D3D12Device.hpp"
namespace RHI {
	static D3D12_INDIRECT_ARGUMENT_TYPE GetIndirectArgumentD3DType(CommandSignature::IndirectArgumentType indirectType) {
		switch (indirectType)
		{
		case CommandSignature::IndirectArgumentType::DARW:
			return D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
		case CommandSignature::IndirectArgumentType::DISPATCH:
			return D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
		case CommandSignature::IndirectArgumentType::DISPATCH_MESH:
			return D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH;
		default:
			return (D3D12_INDIRECT_ARGUMENT_TYPE)-1;
		}
	}
	static size_t GetIndirectArgumentSize(CommandSignature::IndirectArgumentType indirectType) {
		switch (indirectType)
		{
		case CommandSignature::IndirectArgumentType::DARW:
			return sizeof(D3D12_DRAW_ARGUMENTS);
		case CommandSignature::IndirectArgumentType::DISPATCH:
			return sizeof(D3D12_DISPATCH_ARGUMENTS);
		case CommandSignature::IndirectArgumentType::DISPATCH_MESH:
			return sizeof(D3D12_DISPATCH_MESH_ARGUMENTS);
		default:
			return 0;			
		}
	}
	CommandSignature::CommandSignature(Device* device, IndirectArgumentType indirectType) : m_Type(indirectType) {
		D3D12_COMMAND_SIGNATURE_DESC desc{};
		D3D12_INDIRECT_ARGUMENT_DESC argument_desc{};
		desc.NumArgumentDescs = 1;
		argument_desc.Type = GetIndirectArgumentD3DType(indirectType);
		desc.pArgumentDescs = &argument_desc;
		desc.ByteStride = GetIndirectArgumentSize(indirectType);
		CHECK_HR(device->GetNativeDevice()->CreateCommandSignature(&desc, nullptr, IID_PPV_ARGS(&m_Signature)));
	}
}