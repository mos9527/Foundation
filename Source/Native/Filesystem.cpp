#include "Filesystem.hpp"
#include <fstream>
namespace Foundation::Native
{
    size_t GetFileSize(Path const& path)
    {
        return std::filesystem::file_size(path);
    }
    size_t ReadFile(Path const& path, ByteArray& data)
    {
        std::ifstream file(path, std::ios::binary);
        CHECK_MSG(file.good(), "Failed to open file {}", path);
        data.resize(GetFileSize(path));
        file.read(data.data(), static_cast<uint32_t>(data.size()));
        CHECK_MSG(file.gcount() == data.size(), "Read {} bytes, expected {}", file.gcount(), data.size());
        return file.gcount();
    }
    size_t WriteFile(Path const& path, Bytes data)
    {
        std::ofstream file(path, std::ios::binary);
        CHECK_MSG(file.good(), "Failed to open file {}", path);
        file.write(data.data(), static_cast<uint32_t>(data.size()));
        size_t bytes_wrote = file.tellp();
        CHECK_MSG(bytes_wrote == data.size(), "Wrote {} bytes, expected {}", bytes_wrote, data.size());
        return bytes_wrote;
    }
} // namespace Foundation::Native
