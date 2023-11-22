#include "ImageAsset.hpp"

Asset<Bitmap32bpp>::Asset(Bitmap32bpp&& bitmap) {
	initialData = std::move(bitmap);
}

void SDRImageAsset::upload(RHI::Device* device) {		
	texture = std::make_unique<RHI::Texture>(device, RHI::Resource::ResourceDesc::GetTextureBufferDesc(
		RHI::ResourceFormat::R8G8B8A8_UNORM, RHI::ResourceDimension::Texture2D,
		initialData.width, initialData.height, 1, 1, 1, 0
	));
	RHI::Subresource subresource {
		.pSysMem = initialData.data,
		.rowPitch = (uint)initialData.width * 4,
		.slicePitch = 0
	};
	device->Upload(texture.get(), &subresource, 1);
	textureSrv = std::make_unique<RHI::ShaderResourceView>(texture.get(), RHI::ShaderResourceViewDesc::GetTexture2DDesc(
		RHI::ResourceFormat::R8G8B8A8_UNORM, 0, 1
	));
}