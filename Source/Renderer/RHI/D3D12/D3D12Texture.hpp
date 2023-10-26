#pragma once
#include "../RHICommon.hpp"
namespace RHI {
	class Device;
	class Texture {
	public:
		struct GpuTextureDesc {
			enum GpuTextureType {
				Texture1D,
				Texture1DArray,
				Texture2D,
				Texture2DArray,
				Cubemap,
				CubemapArray,
				Texture3D,
				Texture3DArray
			};
		};
		Texture(GpuTextureDesc const& desc, ComPtr<ID3D12Resource>&& backbuffer);
		~Texture() = default;
		inline operator ID3D12Resource* () { return m_Texture.Get(); }
		inline void Reset() { m_Texture.Reset(); }
		inline GpuTextureDesc GetDesc() { return m_Desc; }
	private:
		const GpuTextureDesc m_Desc;
		ComPtr<ID3D12Resource> m_Texture;
	};
}