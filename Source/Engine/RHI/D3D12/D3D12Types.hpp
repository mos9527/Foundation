#pragma once
#include "../../../pch.hpp"
namespace RHI {
#define DEFINE_PLUS_TO_VALUE(T) inline constexpr unsigned int operator+(T const v) { return static_cast<unsigned int>(v); }
	enum class TextureLayout {
		Unknown = D3D12_TEXTURE_LAYOUT_UNKNOWN,
		RowMajor = D3D12_TEXTURE_LAYOUT_ROW_MAJOR
	};
	// DEFINE_ENUM_FLAG_OPERATORS(TextureLayout);
	DEFINE_PLUS_TO_VALUE(TextureLayout);
	enum class ResourceDimension {
		Unknown = D3D12_RESOURCE_DIMENSION_UNKNOWN,
		Buffer = D3D12_RESOURCE_DIMENSION_BUFFER,
		Texture1D = D3D12_RESOURCE_DIMENSION_TEXTURE1D,
		Texture2D = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
		Texture3D = D3D12_RESOURCE_DIMENSION_TEXTURE3D
	};
	// DEFINE_ENUM_FLAG_OPERATORS(ResourceDimension);
	DEFINE_PLUS_TO_VALUE(ResourceDimension);
	enum class ResourceDimensionSRV {
		Buffer = D3D12_SRV_DIMENSION_BUFFER,
		Texture2D = D3D12_SRV_DIMENSION_TEXTURE2D,
	};
	DEFINE_PLUS_TO_VALUE(ResourceDimensionSRV);
	enum class ResourceDimensionUAV {
		Buffer = D3D12_UAV_DIMENSION_BUFFER,
		Texture2D = D3D12_UAV_DIMENSION_TEXTURE2D,
	};	
	DEFINE_PLUS_TO_VALUE(ResourceDimensionUAV);
	enum class ResourceFlags {
		None = D3D12_RESOURCE_FLAG_NONE,
		NoShaderResource = D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE,
		UnorderedAccess = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		RenderTarget = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
		DepthStencil = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
		Unbound
	};
	DEFINE_ENUM_FLAG_OPERATORS(ResourceFlags);
	DEFINE_PLUS_TO_VALUE(ResourceFlags);
	enum class ResourceState {
		Common = D3D12_RESOURCE_STATE_COMMON,
		CopyDest = D3D12_RESOURCE_STATE_COPY_DEST,
		CopySource = D3D12_RESOURCE_STATE_COPY_SOURCE,
		VertexAndConstantBuffer = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
		IndexBuffer = D3D12_RESOURCE_STATE_INDEX_BUFFER,
		RenderTarget = D3D12_RESOURCE_STATE_RENDER_TARGET,
		Present = D3D12_RESOURCE_STATE_PRESENT,
		UnorderedAccess = D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		NonPixelShaderResoruce = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
		PixelShaderResource = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
		// VertexAndConstantBuffer + IndexBuffer + SRV + IndirectArgument + Copy
		GenericRead = D3D12_RESOURCE_STATE_GENERIC_READ,
		DepthWrite = D3D12_RESOURCE_STATE_DEPTH_WRITE,
		DepthRead = D3D12_RESOURCE_STATE_DEPTH_READ,
		AccleartionStruct = D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE
	};
	DEFINE_ENUM_FLAG_OPERATORS(ResourceState);
	DEFINE_PLUS_TO_VALUE(ResourceState);
	enum class ResourceHeapType {
		Default = 0,
		Upload = 1,
		Readback = 2
	};
	DEFINE_PLUS_TO_VALUE(ResourceHeapType);
	inline static D3D12_HEAP_TYPE ResourceHeapTypeToD3DHeapType(ResourceHeapType usage) {
		switch (usage) {
		case ResourceHeapType::Upload:
			return D3D12_HEAP_TYPE_UPLOAD;			
		case ResourceHeapType::Readback:
			return D3D12_HEAP_TYPE_READBACK;		
		default:
		case ResourceHeapType::Default:
			return D3D12_HEAP_TYPE_DEFAULT;			
		};
	}
	enum class ResourceUsage {
		ShaderResource = 1 << 0,
		UnorderedAccess = 1 << 1,
		RenderTarget = 1 << 2,
		DepthStencil = 1 << 3
	};
	DEFINE_ENUM_FLAG_OPERATORS(ResourceUsage);
	enum class ResourceFormat {
		Unknown = DXGI_FORMAT_UNKNOWN,
		R8G8B8A8_UNORM = DXGI_FORMAT_R8G8B8A8_UNORM,
		R16G16B16A16_UNORM = DXGI_FORMAT_R16G16B16A16_UNORM,
		R32_FLOAT = DXGI_FORMAT_R32_FLOAT,
		D32_FLOAT = DXGI_FORMAT_D32_FLOAT
	};
	DEFINE_PLUS_TO_VALUE(ResourceFormat);
	inline static DXGI_FORMAT ResourceFormatToD3DFormat(ResourceFormat format) {
		return (DXGI_FORMAT)format;
	}
	constexpr size_t GetResourceFormatWidth(ResourceFormat format) {
		switch (format)
		{
		case ResourceFormat::R8G8B8A8_UNORM:
			return 4;
		case ResourceFormat::R16G16B16A16_UNORM:
			return 8;
		case ResourceFormat::Unknown:
		default:
			return -1;		
		}
	}
	enum class ResourcePoolType {
		Default = 0,
		Intermediate = 1, /* resources can only be uploaded here by the CPU (TYPE_UPLOAD). its contents are managed linearly since they are expected to be all freed at once */
		NUM_TYPES = 2
	};
	DEFINE_PLUS_TO_VALUE(ResourcePoolType);
	enum class CommandListType
	{
		Direct = 0,
		Compute = 1,
		Copy = 2,
		NUM_TYPES = 3
	};
	DEFINE_PLUS_TO_VALUE(CommandListType);
	static inline D3D12_COMMAND_LIST_TYPE CommandListTypeToD3DType(CommandListType type) {
		switch (type)
		{
		case CommandListType::Direct:
			return D3D12_COMMAND_LIST_TYPE_DIRECT;
		case CommandListType::Compute:
			return D3D12_COMMAND_LIST_TYPE_COMPUTE;
		case CommandListType::Copy:
			return D3D12_COMMAND_LIST_TYPE_COPY;
		default:
			return (D3D12_COMMAND_LIST_TYPE)-1;
		}
	}
	enum class DescriptorHeapType {
		CBV_SRV_UAV = 0,
		SAMPLER = 1,
		RTV = 2,
		DSV = 3,
		NUM_TYPES = 4
	};
	DEFINE_PLUS_TO_VALUE(DescriptorHeapType);
	static D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapTypeToD3DType(DescriptorHeapType type) {
		switch (type)
		{
		case DescriptorHeapType::CBV_SRV_UAV:
			return D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		case DescriptorHeapType::SAMPLER:
			return D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
		case DescriptorHeapType::RTV:
			return D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		case DescriptorHeapType::DSV:
			return D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
		default:
			return (D3D12_DESCRIPTOR_HEAP_TYPE)-1;
		}
	}
	enum class IndirectArgumentType {
		DARW = 0,
		DISPATCH = 1,
		DISPATCH_MESH = 2,
		NUM_TYPES = 3
	};
	DEFINE_PLUS_TO_VALUE(IndirectArgumentType);
	static D3D12_INDIRECT_ARGUMENT_TYPE IndirectArgumentToD3DType(IndirectArgumentType indirectType) {
		switch (indirectType)
		{
		case IndirectArgumentType::DARW:
			return D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
		case IndirectArgumentType::DISPATCH:
			return D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
		case IndirectArgumentType::DISPATCH_MESH:
			return D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH;
		default:
			return (D3D12_INDIRECT_ARGUMENT_TYPE)-1;
		}
	}
	static size_t IndirectArgumentToSize(IndirectArgumentType indirectType) {
		switch (indirectType)
		{
		case IndirectArgumentType::DARW:
			return sizeof(D3D12_DRAW_ARGUMENTS);
		case IndirectArgumentType::DISPATCH:
			return sizeof(D3D12_DISPATCH_ARGUMENTS);
		case IndirectArgumentType::DISPATCH_MESH:
			return sizeof(D3D12_DISPATCH_MESH_ARGUMENTS);
		default:
			return 0;
		}
	}
	struct DepthStencilValue {
		FLOAT depth;
		UINT8 stencil;
	};
	struct ClearValue {		
		union {
			FLOAT color[4];
			DepthStencilValue depthStencil;
		};

		const D3D12_CLEAR_VALUE ToD3D12ClearValue(ResourceFormat format) const {
			D3D12_CLEAR_VALUE value{ .Format = ResourceFormatToD3DFormat(format)};
			memcpy_s(&value.Color, sizeof(value.Color), &color, sizeof(color));
			return value;
		}
	};
	typedef D3D12_SHADER_RESOURCE_VIEW_DESC ShaderResourceViewDesc; // xxx Do RHI implementations for these as well
	typedef D3D12_DEPTH_STENCIL_VIEW_DESC DepthStencilViewDesc;
	typedef D3D12_RENDER_TARGET_VIEW_DESC RenderTargetViewDesc;
	typedef D3D12_UNORDERED_ACCESS_VIEW_DESC UnorderedAccessViewDesc;

	class Device;
	class DeviceChild {
	protected:
		Device* m_Device;
	public:
		DeviceChild(Device* device) : m_Device(device) {};
		Device* GetParent() { return m_Device; }
	};

	template<typename T> concept Releaseable = requires(T obj) { obj.release(); };
}