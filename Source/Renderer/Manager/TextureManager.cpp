#include "TextureManager.hpp"

entt::entity TextureManager::LoadTexture(RHI::Device* device, RHI::CommandList* cmdList, IO::bitmap8bpp& bmp) {
	RHI::SubresourceData subresource{
		.pSysMem = bmp.data,
		.rowPitch = bmp.width * 4u,
		.slicePitch = bmp.width * bmp.height * 4u
	};
	auto desc = RHI::Texture::TextureDesc::GetTextureBufferDesc(
		RHI::ResourceFormat::R8G8B8A8_UNORM,
		RHI::ResourceDimension::Texture2D,
		bmp.width,
		bmp.height,
		1, 1
	);
	auto texture = std::make_unique<RHI::Texture>(
		device,
		desc,
		cmdList,
		&subresource,
		1
	);

	auto entitiy = registery.create();		
	registery.emplace<std::unique_ptr<RHI::Texture>>(entitiy, std::move(texture));
	return entitiy;
}