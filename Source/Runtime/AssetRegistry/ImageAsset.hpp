#pragma once
#include "IO.hpp"
#include "../RHI/RHI.hpp"
#include "Asset.hpp"
#include "ImageImporter.hpp"

#include "../../Runtime/Renderer/Shaders/Shared.h"
struct ImageAsset {
	std::unique_ptr<RHI::Texture> texture;
	std::unique_ptr<RHI::ShaderResourceView> textureSrv;
};
template<> struct Asset<Bitmap32bpp> : public ImageAsset {
	using imported_type = Bitmap32bpp;
	static constexpr AssetType type = AssetType::Image;
	
	Bitmap32bpp initialData;

	Asset(Bitmap32bpp&&);
	void upload(RHI::Device*);
	void clean() {
		initialData.free();
	}
};
typedef Asset<Bitmap32bpp> SDRImageAsset;