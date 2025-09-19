#include <RenderCore/Shader.hpp>
#include <Core/DefaultAllocator.hpp>
#include <fstream>
#include <iostream>
using namespace Foundation::RenderCore;

DefaultAllocator g_alloc;
Vector<char> ReadFile(std::string const& path) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    CHECK(file.good());
    Vector<char> data(g_alloc.Ptr());
    data.resize(file.tellg());
    file.seekg(0, std::ios::beg);
    file.read(data.data(), static_cast<std::streamsize>(data.size()));
    file.close();
    return data;
}

int main() {
    auto bytecode = ReadFile("data\\shaders\\Triangle_fragMain.spirv");
    Shader refl(bytecode, &g_alloc);
    std::cout << refl.DbgDumpShaderInfo() << std::endl;
}
