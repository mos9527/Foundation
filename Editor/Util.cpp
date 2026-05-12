#include "Scene/Scene.hpp"
#include "Scene/Texture.hpp"
int main_scene(StringView srcPath, StringView dstPath)
{
    MemoryMappedFile file(dstPath, 64ull * 1024ull * 1024ull);
    FScene scene(file);
    LOG(Util, LogDebug, "Saving");
    LoadGLTF(srcPath, scene);
    return 0;
}
int main_texture(StringView srcImagePath, StringView dstDDSPath)
{
    FTexture texture(GLOBAL_ALLOC);
    LoadRGBA8(texture, srcImagePath);
    texture.GenerateMips();
    texture = texture.EncodeBC7();
    uint64_t fileSize = sizeof(texture.magic) + sizeof(texture.header) +
                        (texture.header.ddspf.fourCC == DDSPF_DX10.fourCC ? sizeof(texture.header10) : 0) +
                        texture.bytes.size();
    MemoryMappedFile file(dstDDSPath, fileSize);
    SpanWriter writer(file.MutableBytes());
    FSerialize(writer, texture);
    CHECK(writer.tell() == fileSize);
    file.Flush();
    return 0;
}
int main(int argc, const char** argv)
{
    if (argc == 1)
        goto END;
    if (strcmp(argv[1], "scene") == 0)
    {
        CHECK_MSG(argc - 2 == 2, "usage: util scene [src] [dst]");
        return main_scene(argv[2], argv[3]);
    }
    if (strcmp(argv[1], "texture") == 0)
    {
        CHECK_MSG(argc - 2 == 2, "usage: util texture [src image path] [dst DDS path]");
        return main_texture(argv[2], argv[3]);
    }
END:
    fmt::println("available tools:");
    fmt::println("\tscene");
    fmt::println("\ttexture");
    fmt::println("run 'util [tool name]' for more info");
    return 1;
}
