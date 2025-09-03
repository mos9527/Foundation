#include <Core/Allocator/DefaultAllocator.hpp>
#include <Renderer/ShaderReflection.hpp>
#include <fstream>
#include <iostream>
using namespace Foundation;
using namespace Foundation::Core;
std::vector<char> ReadFile(std::string const& path) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    CHECK(file.good());
    std::vector<char> data;
    data.resize(file.tellg());
    file.seekg(0, std::ios::beg);
    file.read(data.data(), static_cast<std::streamsize>(data.size()));
    file.close();
    return data;
}
DefaultAllocator g_alloc;
int main() {
    auto bytecode = ReadFile(".derived\\shaders\\Triangle_fragMain.spirv");
    ShaderReflection refl(bytecode, &g_alloc);
    std::cout << refl.DbgDumpShaderInfo() << std::endl;
}
