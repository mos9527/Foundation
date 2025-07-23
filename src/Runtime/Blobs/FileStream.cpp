#include "FileStream.hpp"
using namespace Foundation::Blobs;
FileStream::FileStream(std::filesystem::path const& path, std::ios_base::openmode mode) {
    m_file.open(path, mode);
    if (!m_file.is_open()) {
        throw std::runtime_error("Failed to open file: " + path.string());
    }
}
size_t FileStream::Read(byte_span dst) {
    if (!m_file.is_open()) return 0;
    m_file.read(dst.data(), dst.size());
    return m_file.gcount();
}
size_t FileStream::Write(const_byte_span src) {
    if (!m_file.is_open()) return 0;
    m_file.write(src.data(), src.size());
    return src.size();
}
size_t FileStream::Seek(size_t offset, std::ios_base::seekdir origin) {
    if (!m_file.is_open()) return 0;
    m_file.seekg(offset, origin);
    return m_file.tellg();
}
size_t FileStream::Tell() {
    if (!m_file.is_open()) return 0;
    return m_file.tellg();
}
void FileStream::Flush() {
    if (!m_file.is_open()) return;
    m_file.flush();
}
