#pragma once
#include <Core/Core.hpp>
#include <Bits/Chrono.hpp>
#include <filesystem>
namespace Foundation::Native
{
    using namespace Core;
    using Path = std::filesystem::path;
    using ByteVector = Vector<char>;

    size_t ReadFile(Path const& path, ByteVector& data);
}