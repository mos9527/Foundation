#pragma once
#include "IO.hpp"
namespace IO {
	struct Bitmap8bpp {
		uint8_t* data;
		int width;
		int height;
		int channels;	
		~Bitmap8bpp() {
			if (data) ::free(data);
		}
	};
	Bitmap8bpp LoadBitmap8bpp(const char* filepath);
	Bitmap8bpp LoadBitmap8bpp(path_t filepath);
}