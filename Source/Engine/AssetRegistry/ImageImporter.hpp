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