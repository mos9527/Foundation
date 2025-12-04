#include <fstream>
#include "Scene.hpp"
int main_scene(StringView srcPath, StringView dstPath)
{
    Vector<FMesh> meshes(GLOBAL_ALLOC);
    Vector<FInstance> instances(GLOBAL_ALLOC);
    Vector<FCamera> cameras(GLOBAL_ALLOC);
    SceneLoadGLTF(srcPath, meshes, instances, cameras);
    LOG(Util, LogDebug, "Compressing data");
    {
        ThreadPool pool(std::thread::hardware_concurrency(), ThreadPool::getTaskSize(meshes.size()), GLOBAL_ALLOC);
        // Compress all meshes
        for (auto& mesh : meshes)
            pool.Push([&] { mesh.EnsureCompressed(); });
        pool.Join();
    }
    LOG(Util, LogDebug, "Saving");
    SceneSaveBinFile(dstPath, meshes, instances, cameras);
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
END:
    fmt::println("available tools:");
    fmt::println("\tscene");
    fmt::println("run 'util [tool name]' for more info");
    return 1;
}
