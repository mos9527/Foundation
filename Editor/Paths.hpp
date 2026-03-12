#pragma once
#include <Core/Paths.hpp>

// Convenience shim so Editor code can use Paths::Resolve / Paths::Init
// instead of the fully-qualified Foundation::Core names.
namespace Paths {
    inline void Init(const char* argv0) { Foundation::Core::PathsInit(argv0); }
    inline void InitFromDir(const char* exeDir) { Foundation::Core::PathsInitFromDir(exeDir); }
    inline Foundation::Core::String Resolve(Foundation::Core::StringView relPath)
    {
        return Foundation::Core::PathsResolve(relPath);
    }
} // namespace Paths
