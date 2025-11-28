#include "Scene.hpp"
#include <fstream>
int main_scene(StringView srcPath, StringView dstPath)
{
    Vector<FMesh> meshes(GLOBAL_ALLOC);
    Vector<FInstance> instances(GLOBAL_ALLOC);
    Vector<FCamera> cameras(GLOBAL_ALLOC);
    LoadGLTF(srcPath, meshes, instances, cameras);
    FWriter writer(GLOBAL_ALLOC);
    LOG(UtilScene, LogDebug, "Serializing data");
    SceneSerialize(writer, meshes, instances, cameras);
    LOG(UtilScene, LogDebug, "Size={}", writer.buffer.size());
    // Write to file
    std::ofstream f(dstPath.data(), std::ios::binary);
    CHECK_MSG(f.write(writer.buffer.data(), writer.buffer.size()), "Failed to write data");
    return 0;
}
int main(int argc, const char** argv)
{
    if (strcmp(argv[1], "scene") == 0)
    {
        CHECK_MSG(argc - 2 == 2, "usage: util scene [src] [dst]");
        return main_scene(argv[2], argv[3]);
    }
    fmt::println("available tools:");
    fmt::println("\tscene");
    fmt::println("run 'util [tool name]' for more info");
    return 1;
}
