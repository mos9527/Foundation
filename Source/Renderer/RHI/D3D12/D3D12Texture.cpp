#include "D3D12Texture.hpp"
namespace RHI {
	Texture::Texture(GpuTextureDesc const& desc, ComPtr<ID3D12Resource>&& backbuffer) : m_Desc(desc), m_Texture(backbuffer) {};
}