#include "TextureManager.hpp"
namespace RHI {
	TextureManager::TextureManager() {
		m_HandleQueue.Setup(MAX_NUM_TEXTURES);
	}

	void TextureManager::Free(handle_type handle) {
		m_Textures[handle].reset();
		m_HandleQueue.Return(handle);
	}

	handle_type TextureManager::LoadTexture(Device* device, Bitmap8bpp bmp) {
		SubresourceData subresource{
			.pSysMem = bmp.data,
			.rowPitch = bmp.width * sizeof(*bmp.data) * 4,
			.slicePitch = 0
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
			device->GetCommandList(CommandList::COMPUTE),
			&subresource,
			1
		);
		m_Textures.push_back(std::move(texture));
		return m_HandleQueue.Pop();
	}
}