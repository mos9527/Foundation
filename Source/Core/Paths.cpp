#include "Paths.hpp"
#include <filesystem>

namespace Foundation::Core {

static std::filesystem::path sExeDir;

void PathsInit(const char* argv0)
{
    sExeDir = std::filesystem::weakly_canonical(argv0).parent_path();
}

void PathsInitFromDir(const char* exeDir)
{
    sExeDir = std::filesystem::path(exeDir);
}

String PathsResolve(StringView relPath)
{
    std::filesystem::path p(relPath.data());
    if (p.is_absolute())
        return String(relPath);
    return String((sExeDir / p).string());
}

} // namespace Foundation::Core
