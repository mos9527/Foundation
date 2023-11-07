#include "MaterialManager.hpp"

entt::entity MaterialManager::LoadTexture(RHI::Device* device, RHI::CommandList* cmdList, RHI::DescriptorHeap* destHeap, IO::bitmap8bpp& bmp) {
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
	auto buffer = std::make_unique<RHI::Texture>(
		device,
		desc,
		cmdList,
		&subresource,
		1
	);
	Material material = {
		.name = "Texture",
		.texture_buffer = std::move(buffer)
	};
	
	auto entity = registry.create();
	registry.emplace<Material>(entity, std::move(material));
	registry.emplace<Tag>(entity, Tag::Texture);
	return entity;
}