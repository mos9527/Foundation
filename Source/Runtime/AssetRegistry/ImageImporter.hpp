#pragma once
#include "IO.hpp"

struct Bitmap8bpp {
	uint8_t* data;
	int width;
	int height;
	int channels;
	const static size_t texel = 4 * 1;
	const size_t size_in_bytes() const {
		return width * height * texel;
	}
	~Bitmap8bpp() {
		if (data) ::free(data);
	}
};
inline Bitmap8bpp load_bitmap_8bpp(uint8_t* mem, size_t length) {
	int width, height, channels;
	uint8_t* data = stbi_load_from_memory(mem, length, &width, &height, &channels, STBI_rgb_alpha);
	CHECK(data);
	return { data, width, height, channels }; // mandatory RVO
};
inline Bitmap8bpp load_bitmap_8bpp(path_t filepath) {
	filepath = get_absolute_path(filepath);
	FILE* f; 
	_wfopen_s(&f, filepath.c_str(), L"rb");
	CHECK(f != nullptr);
	int width, height, channels;
	uint8_t* data = stbi_load_from_file(f, &width, &height, &channels, STBI_rgb_alpha);
	CHECK(data);
	return { data, width, height, channels };
}