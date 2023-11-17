#include "ImageAsset.hpp"

Asset<Bitmap8bpp>::Asset(Bitmap8bpp&& bitmap) {
	initialData = std::move(bitmap);
}

void SDRImageAsset::upload(RHI::Device* device) {	
	uint mipLevels = std::floor(std::log2(std::min(initialData.width, initialData.height))) + 1;
	texture = std::make_unique<RHI::Texture>(device, RHI::Resource::ResourceDesc::GetTextureBufferDesc(
		RHI::ResourceFormat::R8G8B8A8_UNORM, RHI::ResourceDimension::Texture2D,
		initialData.width, initialData.height, mipLevels, 1, 1, 0
	));
	device->Upload(texture.get(), initialData.data, initialData.size_in_bytes());
}