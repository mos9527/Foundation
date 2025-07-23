#pragma once
#include "Blobs.hpp"
#include <filesystem>
#include <fstream>
namespace Foundation::Blobs {
    class FileStream : public Stream {
        std::fstream m_file;
    public:
        FileStream(std::filesystem::path const& path, std::ios_base::openmode mode);
        ~FileStream() = default;
        size_t Read(byte_span span) override;
        size_t Write(const_byte_span span) override;
        size_t Seek(size_t offset, std::ios_base::seekdir) override;
        size_t Tell() override;
        void Flush() override;
    };
}
