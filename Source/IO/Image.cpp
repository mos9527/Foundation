#define STB_IMAGE_IMPLEMENTATION
#define STBI_WINDOWS_UTF8
#include "../../Dependencies/stb_image.h"
#include "../Renderer/RHI/ResourceTypes.hpp"
#include "Image.hpp"

static Bitmap8bpp LoadBitmap(FILE* f) {
	Bitmap8bpp bmp;
	bmp.data = stbi_load_from_file(f, &bmp.width, &bmp.height, &bmp.channels, STBI_rgb_alpha);
	CHECK(bmp.data);
}
static Bitmap8bpp LoadBitmap(const char* filepath) {
	FILE* f = stbi__fopen(filepath, "rb");
	CHECK(f);
	Bitmap8bpp bmp = LoadBitmap(f);
	fclose(f);
	return bmp;
}