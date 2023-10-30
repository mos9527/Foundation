#pragma once
namespace RHI {
	enum class TextureLayout {
		Unknown = D3D12_TEXTURE_LAYOUT_UNKNOWN,
		RowMajor = D3D12_TEXTURE_LAYOUT_ROW_MAJOR
	};
	// DEFINE_ENUM_FLAG_OPERATORS(TextureLayout);
	enum class ResourceDimension {
		Unknown = D3D12_RESOURCE_DIMENSION_UNKNOWN,
		Buffer = D3D12_RESOURCE_DIMENSION_BUFFER,
		Texture1D = D3D12_RESOURCE_DIMENSION_TEXTURE1D,
		Texture2D = D3D12_RESOURCE_DIMENSION_TEXTURE2D,
		Texture3D = D3D12_RESOURCE_DIMENSION_TEXTURE3D
	};
	// DEFINE_ENUM_FLAG_OPERATORS(ResourceDimension);
	enum class ResourceDimensionSRV {
		Buffer = D3D12_SRV_DIMENSION_BUFFER,
		Texture2D = D3D12_SRV_DIMENSION_TEXTURE2D,
	};
	enum class ResourceDimensionUAV {
		Buffer = D3D12_UAV_DIMENSION_BUFFER,
		Texture2D = D3D12_UAV_DIMENSION_TEXTURE2D,
	};	
	enum class ResourceFlags {
		None = D3D12_RESOURCE_FLAG_NONE,
		NoShaderResource = D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE,
		UnorderedAccess = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
		RenderTarget = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
		DepthStencil = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,
		Unbound
	};
	DEFINE_ENUM_FLAG_OPERATORS(ResourceFlags);
	enum class ResourceState {
		Common = D3D12_RESOURCE_STATE_COMMON,
		CopyDest = D3D12_RESOURCE_STATE_COPY_DEST,
		CopySource = D3D12_RESOURCE_STATE_COPY_SOURCE,
		VertexAndConstantBuffer = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER,
		IndexBuffer = D3D12_RESOURCE_STATE_INDEX_BUFFER,
		RenderTarget = D3D12_RESOURCE_STATE_RENDER_TARGET,
		UnorderedAccess = D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
		NonPixelShaderResoruce = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
		PixelShaderResource = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
		// VertexAndConstantBuffer + IndexBuffer + SRV + IndirectArgument + Copy
		GenericRead = D3D12_RESOURCE_STATE_GENERIC_READ
	};
	DEFINE_ENUM_FLAG_OPERATORS(ResourceState);
	enum class ResourceUsage {
		Default,
		Upload,
		Readback
	};
	inline static D3D12_HEAP_TYPE ResourceUsageToD3DHeapType(ResourceUsage usage) {
		switch (usage) {
		case ResourceUsage::Upload:
			return D3D12_HEAP_TYPE_UPLOAD;			
		case ResourceUsage::Readback:
			return D3D12_HEAP_TYPE_READBACK;		
		default:
		case ResourceUsage::Default:
			return D3D12_HEAP_TYPE_DEFAULT;			
		};
	}
	enum class ResourceFormat {
		Unknown = DXGI_FORMAT_UNKNOWN,
		R8G8B8A8_UNORM = DXGI_FORMAT_R8G8B8A8_UNORM,
		R16G16B16A16_UNORM = DXGI_FORMAT_R16G16B16A16_UNORM
	};
	DEFINE_ENUM_FLAG_OPERATORS(ResourceFormat);
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
	struct DepthStencilValue {
		FLOAT depth;
		UINT8 stencil;
	};
	struct ClearValue {
		bool clear{ false };
		union {
			FLOAT color[4];
			DepthStencilValue depthStencil;
		};
		operator D3D12_CLEAR_VALUE() const {
			D3D12_CLEAR_VALUE value{ .Format = DXGI_FORMAT_UNKNOWN };
			memcpy_s(&value.Color, sizeof(value.Color), &color, sizeof(color));
			return value;
		}
	};
}