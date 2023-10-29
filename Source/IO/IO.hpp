#pragma once
#include "../pch.hpp"
namespace IO {
	typedef std::filesystem::path path_t;
	inline bool FileExisits(path_t path) {		
		return std::filesystem::exists(path);
	}
	inline path_t FileAbsolutePath(path_t path) {
		std::filesystem::path p = path;
		return std::filesystem::absolute(p).u8string();
	}
}

#include "Image.hpp"