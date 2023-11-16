#pragma once
#include "IO.hpp"
struct bitmap8bpp {
	uint8_t* data;
	int width;
	int height;
	int channels;
	~bitmap8bpp() {
		if (data) ::free(data);
	}
};
bitmap8bpp load_bitmap_8bpp(path_t filepath);