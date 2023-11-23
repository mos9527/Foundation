#pragma once
#include "IO.hpp"

struct Bitmap32bpp {
	uint8_t* data = nullptr;
	int width;
	int height;
	int channels;
	const static size_t texel = 4 * 1;
	const size_t size_in_bytes() const {
		return width * height * texel;
	}
	const size_t row_pitch() {
		return width * texel;
	}
	void free_data() {
		if (data) {
			stbi_image_free(data);
			data = nullptr;
		}
	}
	~Bitmap32bpp() {
		free_data();
	}
};
inline Bitmap32bpp load_bitmap_32bpp(uint8_t* mem, size_t length) {
	int width, height, channels;
	uint8_t* data = stbi_load_from_memory(mem, length, &width, &height, &channels, STBI_rgb_alpha);
	CHECK(data);
	return Bitmap32bpp(data, width, height, channels);
};
inline Bitmap32bpp load_bitmap_32bpp(path_t filepath) {
	filepath = get_absolute_path(filepath);
	FILE* f; 
	_wfopen_s(&f, filepath.c_str(), L"rb");
	CHECK(f != nullptr);
	int width, height, channels;
	uint8_t* data = stbi_load_from_file(f, &width, &height, &channels, STBI_rgb_alpha);
	CHECK(data);
	return Bitmap32bpp(data, width, height, channels);
}