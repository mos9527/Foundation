#pragma once
#include "../pch.hpp"
using namespace DirectX::SimpleMath;
namespace IO {
	typedef std::filesystem::path path_t;
	inline bool check_file_exisits(path_t path) {
		return std::filesystem::exists(path);
	}
	inline path_t get_absolute_path(path_t path) {
		std::filesystem::path p = path;
		return std::filesystem::absolute(p);
	}
}