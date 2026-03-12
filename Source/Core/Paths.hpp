#pragma once
#include <Core/Container.hpp>

namespace Foundation::Core {

// Init from argv[0] — call as the very first line of main(), before any chdir().
void PathsInit(const char* argv0);

// Init from a directory path directly (e.g. SDL_GetBasePath()).
void PathsInitFromDir(const char* exeDir);

// Prepend exe-dir to a relative path. Absolute paths are returned unchanged.
String PathsResolve(StringView relPath);

} // namespace Foundation::Core
