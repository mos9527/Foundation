#include "TextureManager.hpp"
namespace RHI {
	TextureManager::TextureManager() {
		m_HandleQueue.Setup(MAX_NUM_TEXTURES);
		m_Textures.resize(MAX_NUM_TEXTURES);
		m_TextureDescriptors.resize(MAX_NUM_TEXTURES);
	}

	void TextureManager::Free(handle_type handle) {
		m_Textures[handle].reset();
		m_HandleQueue.Return(handle);
	}

	handle_type TextureManager::LoadTexture(Device* device, CommandList* cmdList, IO::Bitmap8bpp& bmp) {
		SubresourceData subresource{
			.pSysMem = bmp.data,
			.rowPitch = bmp.width * sizeof(*bmp.data) * 4,
			.slicePitch = bmp.width * sizeof(*bmp.data) * 4 * bmp.height
		};
		auto desc = Texture::TextureDesc::GetTextureBufferDesc(
			ResourceFormat::R8G8B8A8_UNORM,
			ResourceDimension::Texture2D,
			bmp.width,
			bmp.height,
			1, 1
		);
		auto texture = std::make_unique<Texture>(
			device, 
			desc,
			cmdList,
			&subresource,
			1
		);
		texture->SetState(cmdList, ResourceState::PixelShaderResource);
		auto srv = device->GetShaderResourceView(texture.get(), ResourceDimensionSRV::Texture2D);
		auto handle = m_HandleQueue.Pop();
		m_Textures[handle] = std::move(texture);
		m_TextureDescriptors[handle] = srv;
		LogD3D12MABudget(device->GetAllocator());
		return handle;
	}
}