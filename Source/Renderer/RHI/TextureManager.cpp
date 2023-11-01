#include "TextureManager.hpp"
namespace RHI {
	TextureManager::TextureManager() {
		m_HandleQueue.setup(MAX_NUM_TEXTURES);
		m_Textures.resize(MAX_NUM_TEXTURES);
		m_TextureSRVs.resize(MAX_NUM_TEXTURES);
	}

	void TextureManager::Free(handle handle) {
		m_Textures[handle].reset();
		m_HandleQueue.push(handle);
	}

	handle TextureManager::LoadTexture(Device* device, CommandList* cmdList, IO::bitmap8bpp& bmp) {
		SubresourceData subresource{
			.pSysMem = bmp.data,
			.rowPitch = bmp.width * 4u,
			.slicePitch = bmp.width * bmp.height * 4u
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
		auto srv = device->GetTexture2DShaderResourceView(texture.get(), ResourceDimensionSRV::Texture2D);
		auto handle = m_HandleQueue.pop();
		texture->SetName(std::format(L"Texture #{}", handle));
		m_Textures[handle] = std::move(texture);
		m_TextureSRVs[handle] = srv;
		return handle;
	}
}