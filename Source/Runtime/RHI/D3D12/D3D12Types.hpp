#pragma once
#include "../../../pch.hpp"
namespace RHI {
#define DEFINE_PLUS_TO_VALUE(T) inline const unsigned int operator+(T const v) { return static_cast<unsigned int>(v); }
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
		AccleartionStruct = D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE,
		IndirectArgument = D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT
	};
	DEFINE_ENUM_FLAG_OPERATORS(ResourceState);
	DEFINE_PLUS_TO_VALUE(ResourceState);
	enum class ResourceHeapType {
		Default = 0,
		Upload = 1,
		Readback = 2
	};
	DEFINE_PLUS_TO_VALUE(ResourceHeapType);
	inline const D3D12_HEAP_TYPE ResourceHeapTypeToD3DHeapType(ResourceHeapType usage) {
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
		R8G8B8A8_UINT = DXGI_FORMAT_R8G8B8A8_UINT,
		R16G16B16A16_UNORM = DXGI_FORMAT_R16G16B16A16_UNORM,
		R32G32B32A32_FLOAT = DXGI_FORMAT_R32G32B32A32_FLOAT,
		R32G32B32_FLOAT = DXGI_FORMAT_R32G32B32_FLOAT,
		R32G32_FLOAT = DXGI_FORMAT_R32G32_FLOAT,
		R16G16_FLOAT = DXGI_FORMAT_R16G16_FLOAT,
		R32_FLOAT = DXGI_FORMAT_R32_FLOAT,
		D32_FLOAT = DXGI_FORMAT_D32_FLOAT
	};
	DEFINE_PLUS_TO_VALUE(ResourceFormat);
	inline const DXGI_FORMAT ResourceFormatToD3DFormat(ResourceFormat format) {
		return (DXGI_FORMAT)format;
	}
	inline const size_t GetResourceFormatWidth(ResourceFormat format) {
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
	inline const D3D12_COMMAND_LIST_TYPE CommandListTypeToD3DType(CommandListType type) {
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
	inline const D3D12_DESCRIPTOR_HEAP_TYPE DescriptorHeapTypeToD3DType(DescriptorHeapType type) {
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
	struct DepthStencilValue {
		FLOAT depth;
		UINT8 stencil;
	};
	struct ClearValue {		
		union {
			FLOAT color[4];
			DepthStencilValue depthStencil;
		};
		ClearValue(float depth, UINT8 stencil) {
			depthStencil.depth = depth;
			depthStencil.stencil = stencil;
		}
		ClearValue(float r, float g, float b, float a) {
			color[0] = r;
			color[1] = g;
			color[2] = b;
			color[3] = a;
		};
		const D3D12_CLEAR_VALUE ToD3D12ClearValue(ResourceFormat format) const {
			D3D12_CLEAR_VALUE value{ .Format = ResourceFormatToD3DFormat(format)};
			memcpy_s(&value.Color, sizeof(value.Color), &color, sizeof(color));
			return value;
		}
	};
	struct ShaderResourceViewDesc {
		D3D12_SHADER_RESOURCE_VIEW_DESC desc;
		operator D3D12_SHADER_RESOURCE_VIEW_DESC() const {
			return desc;
		}		
		friend bool operator== (const ShaderResourceViewDesc& lhs, const ShaderResourceViewDesc& rhs) {
			return true; // xxx implement operator
		}
		static const ShaderResourceViewDesc GetStructuredBufferDesc(
			UINT64                 FirstElement,
			UINT                   NumElements,
			UINT                   StructureByteStride
		) {
			D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
			desc.Format = DXGI_FORMAT_UNKNOWN;
			desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
			desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			desc.Buffer.FirstElement = FirstElement;
			desc.Buffer.NumElements = NumElements;
			desc.Buffer.StructureByteStride = StructureByteStride;
			desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
			return { desc };
		}
		static const ShaderResourceViewDesc GetRawBufferDesc(
			UINT64                 FirstElement,
			UINT                   NumElements
		) {
			D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
			desc.Format = DXGI_FORMAT_UNKNOWN;
			desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
			desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			desc.Buffer.FirstElement = FirstElement;
			desc.Buffer.NumElements = NumElements;
			desc.Buffer.StructureByteStride = 0;
			desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
			return { desc };
		}
		static const ShaderResourceViewDesc GetTexture2DDesc(
			ResourceFormat viewFormat,
			UINT MostDetailedMip,
			UINT MipLevels
		) {
			D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
			desc.Format = ResourceFormatToD3DFormat(viewFormat);
			desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
			desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
			desc.Texture2D.MostDetailedMip = MostDetailedMip;
			desc.Texture2D.MipLevels = MipLevels;
			return { desc };
		}
	};
	struct DepthStencilViewDesc {
		D3D12_DEPTH_STENCIL_VIEW_DESC desc;
		operator D3D12_DEPTH_STENCIL_VIEW_DESC() const {
			return desc;
		}
		friend bool operator== (const DepthStencilViewDesc& lhs, const DepthStencilViewDesc& rhs) {
			return true;
		}
		static const DepthStencilViewDesc GetTexture2DDepthBufferDesc(
			ResourceFormat viewFormat,			
			UINT mipSlice
		) {
			D3D12_DEPTH_STENCIL_VIEW_DESC desc{};
			desc.Format = ResourceFormatToD3DFormat(viewFormat);
			desc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
			desc.Texture2D.MipSlice = mipSlice;
			return { desc };
		}
	};
	struct RenderTargetViewDesc {	
		D3D12_RENDER_TARGET_VIEW_DESC desc;
		operator D3D12_RENDER_TARGET_VIEW_DESC() const {
			return desc;
		}
		friend bool operator== (const RenderTargetViewDesc& lhs, const RenderTargetViewDesc& rhs) {
			return true;
		}
		static const RenderTargetViewDesc GetTexture2DRenderTargetDesc(
			ResourceFormat viewFormat,
			UINT mipSlice
		) {
			D3D12_RENDER_TARGET_VIEW_DESC desc{};
			desc.Format = ResourceFormatToD3DFormat(viewFormat);
			desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
			desc.Texture2D.MipSlice = mipSlice;
			return { desc };
		}
	};
	struct UnorderedAccessViewDesc {
		D3D12_UNORDERED_ACCESS_VIEW_DESC desc;
		operator D3D12_UNORDERED_ACCESS_VIEW_DESC() const {
			return desc;
		}
		size_t GetCounterOffsetInBytes() const {
			return desc.Buffer.CounterOffsetInBytes;
		}
		bool HasCountedResource() const {
			return GetCounterOffsetInBytes() != 0;
		}
		friend bool operator== (const UnorderedAccessViewDesc& lhs, const UnorderedAccessViewDesc& rhs) {
			return true;
		}
		static const UnorderedAccessViewDesc GetStructuredBufferDesc(
			UINT64                 FirstElement,
			UINT                   NumElements,
			UINT                   StructureByteStride,
			UINT				   CounterOffsetInBytes
		) {
			D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
			desc.Format = DXGI_FORMAT_UNKNOWN;
			desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;			
			desc.Buffer.FirstElement = FirstElement;
			desc.Buffer.NumElements = NumElements;
			desc.Buffer.StructureByteStride = StructureByteStride;
			desc.Buffer.CounterOffsetInBytes = CounterOffsetInBytes;			
			desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
			return { desc };
		}
		static const UnorderedAccessViewDesc GetRawBufferDesc(
			UINT64                 FirstElement,
			UINT                   NumElements,
			UINT				   CounterOffsetInBytes
		) {
			D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
			desc.Format = DXGI_FORMAT_UNKNOWN;
			desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
			desc.Buffer.FirstElement = FirstElement;
			desc.Buffer.NumElements = NumElements;
			desc.Buffer.StructureByteStride = 0;
			desc.Buffer.CounterOffsetInBytes = CounterOffsetInBytes;
			desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
			return { desc };
		}
		static const UnorderedAccessViewDesc GetTexture2DDesc(
			ResourceFormat viewFormat,
			UINT MipSlice,
			UINT PlaneSlice
		) {
			D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
			desc.Format = ResourceFormatToD3DFormat(viewFormat);
			desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
			desc.Texture2D.MipSlice = MipSlice;
			desc.Texture2D.PlaneSlice = PlaneSlice;			
			return { desc };
		}
	};
	struct SamplerDesc {
		D3D12_SAMPLER_DESC desc;
		operator D3D12_SAMPLER_DESC() const {
			return desc;
		}
		friend bool operator== (const SamplerDesc& lhs, const SamplerDesc& rhs) {
			return true;
		}
		static const SamplerDesc GetTextureSamplerDesc(
			uint MaxAnisotropy
		) {
			D3D12_SAMPLER_DESC desc;
			desc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
			desc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
			desc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
			desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
			desc.MinLOD = 0;
			desc.MaxLOD = D3D12_FLOAT32_MAX;
			desc.MipLODBias = 0.0f;
			desc.MaxAnisotropy = MaxAnisotropy;
			desc.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
			return { desc };
		}
	};
	class Device;
	class DeviceChild {
	protected:
		Device* m_Device;
	public:
		DeviceChild(Device* device) : m_Device(device) {};
		Device* GetParent() { return m_Device; }

		DeviceChild(DeviceChild&&) = default;
		DeviceChild(const DeviceChild&) = delete;
	};
	
	template<typename T> concept Releaseable = requires(T obj) { obj.release(); };
}