#pragma once
#include "D3D12Device.hpp"
#include "D3D12Types.hpp"
#include "D3D12DescriptorHeap.hpp"
#define RESOURCE_BARRIER_ALL_SUBRESOURCES D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES 
#define RAW_BUFFER_STRIDE 0u
namespace RHI {
	class Device;
	class CommandList;
	struct Subresource {
		const void* pSysMem;
		uint rowPitch;
		uint slicePitch;
		operator const D3D12_SUBRESOURCE_DATA() const {
			return D3D12_SUBRESOURCE_DATA{
				.pData = pSysMem,
				.RowPitch = rowPitch,
				.SlicePitch = slicePitch
			};
		}
	};	
	class Resource : public DeviceChild {
	public:
		struct ResourceDesc {
			std::optional<ClearValue> clearValue;
			ResourceHeapType heapType{ ResourceHeapType::Default };
			ResourceFormat format{ ResourceFormat::Unknown };
			ResourceDimension dimension{ ResourceDimension::Unknown };
			UINT64 alignment{ 0 };
			UINT64 stride{ 0 };
			UINT64 width{ 0 };
			uint height{ 0 };
			UINT16 mipLevels{ 1 };
			UINT16 arraySize{ 1 };
			uint sampleCount{ 1 };
			uint sampleQuality{ 0 };
			TextureLayout layout{ TextureLayout::Unknown };
			ResourceFlags flags{ ResourceFlags::None };
			ResourceState initialState{ ResourceState::Common };
			const wchar_t* resourceName{ nullptr };

			static const ResourceDesc GetGenericBufferDesc(
				UINT64 size, 
				UINT64 stride = RAW_BUFFER_STRIDE,
				ResourceState initialState = ResourceState::Common,
				ResourceHeapType heapType = ResourceHeapType::Upload, 
				ResourceFlags flags = ResourceFlags::None,
				const wchar_t* resourceName = nullptr
			) {
				return ResourceDesc{
					.heapType = heapType,
					.format = ResourceFormat::Unknown,
					.dimension = ResourceDimension::Buffer,
					.stride = stride,
					.width = size,
					.height = 1,
					.mipLevels = 1,
					.arraySize = 1,
					.sampleCount = 1,
					.sampleQuality = 0,
					.layout = TextureLayout::RowMajor,
					.flags = flags,
					.initialState = initialState,
					.resourceName = resourceName
				};
			}
			static const ResourceDesc GetTextureBufferDesc(
				ResourceFormat format,
				ResourceDimension dimension,
				UINT64 width,
				uint height,
				UINT16 mipLevels,
				UINT16 arraySize,
				uint sampleCount = 1,
				uint sampleQuality = 0,
				ResourceFlags flags = ResourceFlags::None,
				ResourceHeapType heapType = ResourceHeapType::Default,
				ResourceState initialState = ResourceState::Common,
				std::optional<ClearValue> clearValue = {},
				const wchar_t* resourceName = nullptr
			) {
				return ResourceDesc{
					.clearValue = clearValue,
					.heapType = heapType,
					.format = format,
					.dimension = dimension,					
					.width = width,
					.height = height,
					.mipLevels = mipLevels,
					.arraySize = arraySize,
					.sampleCount = sampleCount,
					.sampleQuality = sampleQuality,
					.layout = TextureLayout::Unknown,
					.flags = flags,
					.initialState = initialState,
					.resourceName = resourceName
				};
			}
			operator const D3D12_RESOURCE_DESC() const {
				return D3D12_RESOURCE_DESC{
					.Dimension = (D3D12_RESOURCE_DIMENSION)dimension,
					.Alignment = alignment,
					.Width = width,
					.Height = height,
					.DepthOrArraySize = arraySize,
					.MipLevels = mipLevels,
					.Format = ResourceFormatToD3DFormat(format),
					.SampleDesc = DXGI_SAMPLE_DESC{
						.Count = sampleCount,
						.Quality = sampleQuality
					},
					.Layout = (D3D12_TEXTURE_LAYOUT)layout,
					.Flags = (D3D12_RESOURCE_FLAGS)flags
				};
			}
			inline bool isBuffer() const {
				return dimension == ResourceDimension::Buffer;
			}
			inline bool isRawBuffer() const {
				return isBuffer() && stride == RAW_BUFFER_STRIDE;
			}
			inline bool isTexture() const {
				return !isBuffer();
			}
			inline bool allowUnorderedAccess() const {
				return +(flags & ResourceFlags::UnorderedAccess);
			}
			inline bool allowRenderTarget() const {
				return +(flags & ResourceFlags::RenderTarget);
			}
			inline bool allowDepthStencil() const {
				return +(flags & ResourceFlags::DepthStencil);
			}
			inline size_t sizeInBytes() const {				
				CHECK(dimension == ResourceDimension::Buffer);
				return width;
			}
			static inline uint numMipsOfDimension(uint width, uint height) {
				uint res = std::max(width, height);
				return static_cast<uint>(std::floor(std::log2(res)));
			}
			static inline uint indexSubresource(UINT MipSlice, UINT ArraySlice, UINT PlaneSlice, UINT MipLevels, UINT ArraySize)
			{
				return MipSlice + (ArraySlice * MipLevels) + (PlaneSlice * MipLevels * ArraySize);
			}
			inline uint indexSubresource(UINT MipSlice, UINT ArraySlice, UINT PlaneSlice = 0) const {
				return indexSubresource(MipSlice, ArraySlice, PlaneSlice, mipLevels, arraySize);
			}
			inline uint numSubresources() const {
				UINT planes = 1; // xxx DepthStencil & some formats have 2 planes
				return indexSubresource(0, arraySize, planes - 1, mipLevels, arraySize); // arraysize * miplevels * planes
			}
		};		
		Resource(Device* device, ComPtr<ID3D12Resource>&& texture, name_t name = nullptr);
		Resource(Device* device, ResourceDesc const& desc);		
		~Resource() = default;
		inline ResourceDesc const& GetDesc() { return m_Desc;  }
		
		inline ResourceState GetState(UINT subresource=0) { return m_States[subresource]; }		
		
		void SetBarrier(CommandList* cmd, ResourceState state, uint subresource);
		void SetBarrier(CommandList* cmd, ResourceState state, std::vector<UINT>&& subresources);
		void SetBarrier(CommandList* cmd, ResourceState state);

		inline auto GetGPUAddress() { return m_Resource->GetGPUVirtualAddress(); }
		inline auto GetNativeResource() { return m_Resource.Get(); }

		inline operator ID3D12Resource* () { return m_Resource.Get(); }
		inline void Reset() { m_Resource.Reset(); }		

		/* Upload & Readback only! */		
		void Update(const void* data, size_t size, size_t offset);	
		/* Upload & Readback only! */
		void Map(size_t read_begin, size_t read_end);
		void Map() { Map(0, 0); }
		/* Upload & Readback only! */
		void Unmap();		

		inline auto const& GetName() { return m_Name; }
		inline void SetName(name_t name) { 
			m_Name = name;
			m_Resource->SetName(name);
			if (m_Allocation.Get()) m_Allocation->SetName(name);
		}

	protected:
		name_t m_Name;

		const ResourceDesc m_Desc;
		std::vector<ResourceState> m_States; // States for every subresource

		ComPtr<ID3D12Resource> m_Resource;
		ComPtr<D3D12MA::Allocation> m_Allocation;	

		void* pMappedData{ nullptr };
	};
	class Buffer : public Resource {
	public:
		using Resource::Resource;
	};
	class Texture : public Resource {
	public:
		using Resource::Resource;
		ClearValue const& GetClearValue() const {
			return m_Desc.clearValue.value();
		}
	};
	struct DeferredDeleteResource {
		std::unique_ptr<Resource> resource;
		operator Resource* () { return resource.get(); }
		void Release() {
			resource.reset();
		}
	};	
}