#pragma once
#include "IO.hpp"
#include "../RHI/RHI.hpp"
#include "Asset.hpp"
#include "ImageImporter.hpp"

#include "../../Runtime/Renderer/Shaders/Shared.h"
template<> struct Asset<Bitmap8bpp> {
	using imported_type = Bitmap8bpp;
	static constexpr AssetType type = AssetType::Image;
	
	Bitmap8bpp initialData;

	std::unique_ptr<RHI::Texture> texture;
	std::unique_ptr<RHI::ShaderResourceView> textureSrv;

	Asset(Bitmap8bpp&&);
	void upload(RHI::Device*);
	void clean() {
		if (initialData.data) ::free(initialData.data);
	}
};
typedef Asset<Bitmap8bpp> SDRImageAsset;