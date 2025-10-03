#pragma once
#include <Core/Core.hpp>
#include <Bits/Chrono.hpp>
#include <filesystem>
namespace Foundation::Native
{
    /**
     * @brief OS specific filesystem path. Alias of std::filesystem::path
     */
    using Path = std::filesystem::path;
    /**
     * @brief Vector of bytes. Alias of @ref Core::Vector<char>
     */
    using ByteArray = Core::Vector<char>;
    /**
     * @brief Span of bytes. Alias of @ref Core::Span<const char>
     */
    using Bytes = Core::Span<const char>;
    /**
     * @return The size of the file in bytes.
     */
    size_t GetFileSize(Path const& path);
    /**
     * @brief Reads the entire contents of a file into a byte vector.
     * @return The number of bytes read.
     */
    size_t ReadFile(Path const& path, ByteArray& data);
    /**
     * @brief Writes the entire contents of a byte vector to a file.
     * @return The number of bytes written.
     */
    size_t WriteFile(Path const& path, Bytes data);        
} // namespace Foundation::Native

namespace std::filesystem
{
    inline auto format_as(path const& p) { return p.string(); }
} // namespace std::filesystem
