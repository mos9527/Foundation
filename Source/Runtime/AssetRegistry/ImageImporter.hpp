#pragma once
#include "IO.hpp"

struct Bitmap8bpp {
	uint8_t* data;
	int width;
	int height;
	int channels;
	~Bitmap8bpp() {
		if (data) ::free(data);
	}
};
Bitmap8bpp load_bitmap_8bpp(path_t filepath);
Bitmap8bpp load_bitmap_8bpp(path_t filepath) {
	filepath = get_absolute_path(filepath);
	FILE* f; 
	_wfopen_s(&f, filepath.c_str(), L"rb");
	CHECK(f != nullptr);
	Bitmap8bpp bmp;
	bmp.data = stbi_load_from_file(f, &bmp.width, &bmp.height, &bmp.channels, STBI_rgb_alpha);
	CHECK(bmp.data);
	return bmp;
}