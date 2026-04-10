#include <fstream>
#include "Scene/Scene.hpp"
#include "Scene/Texture.hpp"
int main_scene(StringView srcPath, StringView dstPath)
{
    FScene scene(GLOBAL_ALLOC);
    LoadGLTF(srcPath, scene);
    LOG(Util, LogDebug, "Compressing Mesh Data");
    {
        ThreadPool pool(std::thread::hardware_concurrency(), ThreadPool::getTaskSize(scene.mMeshes.size()), GLOBAL_ALLOC);
        // Compress all meshes
        for (auto& mesh : scene.mMeshes)
            pool.Push([&] { mesh.EnsureCompressed(); });
        pool.Join();
    }
    LOG(Util, LogDebug, "Saving");
    FileWriter writer(dstPath);
    FSerialize(writer, scene);
    return 0;
}
int main_texture(StringView srcImagePath, StringView dstDDSPath)
{
    FTexture2D texture(GLOBAL_ALLOC);
    LoadRGBA8(texture, srcImagePath);
    texture.GenerateMips();
    texture = texture.EncodeBC7();
    FileWriter writer(dstDDSPath);
    FSerialize(writer, texture);
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
