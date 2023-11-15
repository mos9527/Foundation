#define STB_IMAGE_IMPLEMENTATION
#define STBI_WINDOWS_UTF8
#include "../../Dependencies/stb_image.h"
#include "ImageImporter.hpp"
namespace IO {
	bitmap8bpp load_bitmap_8bpp(path_t filepath) {
		filepath = get_absolute_path(filepath);
		FILE* f; _wfopen_s(&f, (const wchar_t*)filepath.u16string().c_str(), L"rb");
		CHECK(f);
		bitmap8bpp bmp;
		bmp.data = stbi_load_from_file(f, &bmp.width, &bmp.height, &bmp.channels, STBI_rgb_alpha);
		CHECK(bmp.data);
		return bmp;
	}
}