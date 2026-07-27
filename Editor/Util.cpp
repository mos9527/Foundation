#include "Scene/Scene.hpp"
#include <Renderer/Texture.hpp>
#include <Core/AllocatorStack.hpp>
#include <RHIVulkan/Application.hpp>
int main_scene(JobSystem* jobs, StringView srcPath, StringView dstPath, FSceneBuildOptions const& buildOptions)
{
    MemoryMappedFile file(dstPath, 64ull * 1024ull * 1024ull /* grows on demand */);
    FImportedScene scene(file, GLOBAL_ALLOC);
    LOG(Util, LogDebug, "Saving");
    VulkanApplication app(GLOBAL_ALLOC, true);
    LoadGLTF(app, jobs, srcPath, scene, GLOBAL_ALLOC, buildOptions);
    return 0;
}
int main_texture(StringView srcImagePath, StringView dstDDSPath)
{
    FTexture texture(GLOBAL_ALLOC);
    LoadRGBA8(texture, srcImagePath);
    texture.GenerateMips();
    texture = texture.EncodeBC7(GLOBAL_ALLOC);
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
    unsigned const hardware = std::thread::hardware_concurrency();
    JobSystem jobs({
        .workerCount = hardware > 1u ? static_cast<size_t>(hardware - 1u) : 1u,
        .maxJobs = 4096,
        .maxBarriers = 64,
        .readyQueueSize = 4096,
        .allocator = GLOBAL_ALLOC,
        .name = "UtilJob",
    });
    if (argc == 1)
        goto END;
    if (strcmp(argv[1], "scene") == 0)
    {
        FSceneBuildOptions buildOptions{};
        const char* srcPath = nullptr;
        const char* dstPath = nullptr;
        for (int i = 2; i < argc; ++i)
        {
            if (strcmp(argv[i], "--no-texture-compression") == 0)
            {
                buildOptions.textureCompression = FSceneTextureCompression::None;
                continue;
            }
            if (!srcPath)
                srcPath = argv[i];
            else if (!dstPath)
                dstPath = argv[i];
            else
                CHECK_MSG(false, "usage: util scene [--no-texture-compression] [src] [dst]");
        }
        CHECK_MSG(srcPath && dstPath, "usage: util scene [--no-texture-compression] [src] [dst]");
        return main_scene(&jobs, srcPath, dstPath, buildOptions);
    }
    if (strcmp(argv[1], "texture") == 0)
    {
        CHECK_MSG(argc - 2 == 2, "usage: util texture [src image path] [dst DDS path]");
        return main_texture(argv[2], argv[3]);
    }
END:
    fmt::println("available tools:");
    fmt::println("\tscene [--no-texture-compression] [src] [dst]");
    fmt::println("\ttexture");
    fmt::println("run 'util [tool name]' for more info");
    return 1;
}
