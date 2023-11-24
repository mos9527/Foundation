#pragma once
#include "Asset.hpp"
#include "../../Runtime/Renderer/Shaders/Shared.h"

#include "../RHI/RHI.hpp"
#include "ResourceContainer.hpp"
#include "UploadContext.hpp"
#include "TextureImporter.hpp"
static const RHI::Resource::ResourceDesc GetDefaultTextureBufferDesc(RHI::ResourceFormat format, uint width, uint height, uint numMips, uint numSlices, RHI::name_t name = nullptr) {
	return RHI::Resource::ResourceDesc::GetTextureBufferDesc(
		format, RHI::ResourceDimension::Texture2D, width, height, numMips, numSlices, 1, 0, RHI::ResourceFlags::None, RHI::ResourceHeapType::Default, RHI::ResourceState::CopyDest, {}, name
	);
}
struct TextureAsset;
struct TextureBuffer {
	friend struct TextureAsset;
private:
	Texture2DContainer loadImageBuffer;
public:
	RHI::Texture texture;
	TextureBuffer(RHI::Device* device, RHI::ResourceFormat format, uint width, uint height, uint numMips = 1, uint numSlices = 1, RHI::name_t name = nullptr) :
		loadImageBuffer(device, GetDefaultTextureBufferDesc(format, width, height, numMips, numSlices,name)),
		texture(device, GetDefaultTextureBufferDesc(format, width, height, numMips, numSlices,name)) {}
	void Clean() { loadImageBuffer.Release(); }
};

struct TextureAsset {
	const static AssetType type = AssetType::Texture;
	TextureBuffer texture;
	std::unique_ptr<RHI::ShaderResourceView> textureSRV;

	// Creates a texture of 1 slice, 1 mip, in RGBA8UNORM format, for this bitmap
	TextureAsset(RHI::Device* device, Bitmap32bpp* bitmap);
	void Upload(UploadContext* ctx);
	void Clean() {
		texture.Clean();
	}
};

template<> struct ImportedAssetTraits<Bitmap32bpp> {
	using imported_by = TextureAsset;
	static constexpr AssetType type = AssetType::Texture;
};