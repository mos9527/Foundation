#include "Paths.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace Foundation::Core {

static std::filesystem::path sExeDir;
static PathsAssetLoader sAssetLoader = nullptr;

void PathsInit(const char* argv0)
{
    sExeDir = std::filesystem::weakly_canonical(argv0).parent_path();
}

void PathsInitFromDir(const char* exeDir)
{
    sExeDir = std::filesystem::path(exeDir);
}

void PathsRegisterAssetLoader(PathsAssetLoader loader)
{
    sAssetLoader = loader;
}

String PathsResolve(StringView relPath)
{
    std::filesystem::path p(relPath.data());
    if (p.is_absolute())
        return String(relPath);
    auto out = sExeDir / p;
    // Always (re)materialize from the platform bundle (e.g. Android APK assets)
    // when a loader is registered. This keeps the on-disk copy fresh across APK
    // updates without an invalidation scheme, and only ever copies files that
    // are actually requested via PathsResolve. Paths that aren't in the bundle
    // (e.g. the writable pipeline-cache path) yield no data and pass through.
    if (sAssetLoader)
    {
        size_t size = 0;
        if (void* data = sAssetLoader(relPath.data(), &size))
        {
            std::error_code ec;
            std::filesystem::create_directories(out.parent_path(), ec);
            std::ofstream file(out, std::ios::binary | std::ios::trunc);
            if (file)
                file.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
            free(data);
        }
    }
    return String(out.string());
}

} // namespace Foundation::Core
