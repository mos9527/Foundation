#include "TextureAsset.hpp"
TextureAsset::TextureAsset(RHI::Device* device, Bitmap32bpp* bitmap) :
	/* Most if not all 8bit image formats are in sRGB space. stb_image loads them this way too. The textures should be linear but with sRGB
	* format we don't have to do the (inverse) gamma correction by ourselves.
	*/
	texture(device, bitmap->linear ? RHI::ResourceFormat::R8G8B8A8_UNORM : RHI::ResourceFormat::R8G8B8A8_UNORM_SRGB, bitmap->width, bitmap->height) {
	texture.loadImageBuffer.WriteSubresource(bitmap->data, bitmap->row_pitch(), bitmap->height, 0, 0);
}
TextureAsset::TextureAsset(RHI::Device* device, BitmapRGBA32F* bitmap) : 
	texture(device, RHI::ResourceFormat::R32G32B32A32_FLOAT, bitmap->width, bitmap->height) {
	texture.loadImageBuffer.WriteSubresource(bitmap->data, bitmap->row_pitch(), bitmap->height, 0, 0);
}
void TextureAsset::Upload(UploadContext* ctx) {	
	auto const& desc = texture.texture.GetDesc();
	ctx->QueueUploadTexture(&texture.texture, &texture.loadImageBuffer, 0, texture.texture.GetDesc().numSubresources());		
	// xxx texture2darray support
	// xxx ^ which would entail DDS support
	textureSRV = std::make_unique<RHI::ShaderResourceView>(&texture.texture, RHI::ShaderResourceViewDesc::GetTexture2DDesc(
		desc.format,
		0,
		desc.mipLevels
	));
	isUploaded = true;
}