#pragma once
#include "../pch.hpp"

typedef std::filesystem::path path_t;
inline path_t get_absolute_path(path_t path) {
	std::filesystem::path p = path;
	return std::filesystem::absolute(p);
}
template<typename... Paths> inline std::optional<path_t> find_file(path_t filename, Paths&&... paths) {
	filename = filename.filename();
	for (auto& path : { paths... }) {
		if (std::filesystem::exists(path / filename))
			return path / filename;
	}
	return {};
}
inline std::unique_ptr<std::vector<unsigned char>> read_data(path_t path) {
	path = get_absolute_path(path);
	std::basic_ifstream<uint8_t> file(path, std::ios::binary);
	CHECK(file.good());
	auto buffer = std::make_unique<std::vector<unsigned char>>();
	std::copy(
		std::istreambuf_iterator<uint8_t>(file),
		std::istreambuf_iterator<uint8_t>(),
		std::back_inserter(*buffer)
	);		
	return std::move(buffer);
}
inline void write_data(path_t path, uint8_t* data, size_t size) {
	std::basic_ofstream<uint8_t> file(path, std::ios::binary);
	file.write(data, size);
}