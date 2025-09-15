#include "Filesystem.hpp"
#include <fstream>
namespace Foundation::Native
{
    size_t ReadFile(Path const& path, ByteVector& data)
    {
        std::ifstream file(path, std::ios::binary);
        CHECK_MSG(file.good(), "Failed to open file {}", path.string());
        data.resize(std::filesystem::file_size(path));
        file.read(data.data(), static_cast<uint32_t>(data.size()));
        CHECK_MSG(file.gcount() == data.size(), "Read {} bytes, expected {}", file.gcount(), data.size());
        return file.gcount();
    }
}
