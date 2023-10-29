#define STB_IMAGE_IMPLEMENTATION
#define STBI_WINDOWS_UTF8
#include "../../Dependencies/stb_image.h"
#include "../Renderer/RHI/ResourceTypes.hpp"
#include "Image.hpp"
namespace IO {
	Bitmap8bpp LoadBitmap8bpp(const char* filepath) {
		path_t fullpath = FileAbsolutePath(filepath);
		CHECK(FileExisits(fullpath));
		return LoadBitmap8bpp(fullpath);
	}
	Bitmap8bpp LoadBitmap8bpp(path_t filepath) {
		FILE* f; _wfopen_s(&f, (const wchar_t*)filepath.u16string().c_str(), L"rb");
		CHECK(f);
		Bitmap8bpp bmp;
		bmp.data = stbi_load_from_file(f, &bmp.width, &bmp.height, &bmp.channels, STBI_rgb_alpha);
		CHECK(bmp.data);
		return bmp;
	}
}