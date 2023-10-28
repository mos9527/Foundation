#pragma once
#include "../pch.hpp"

struct Bitmap8bpp {
	uint8_t* data;
	int width;
	int height;
	int channels;
	void free() {
		if (data) ::free(data);
	}
};
static Bitmap8bpp LoadBitmap(FILE* f);
static Bitmap8bpp LoadBitmap(const char* filepath);