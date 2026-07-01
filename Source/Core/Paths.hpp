#pragma once
#include <Core/Container.hpp>

namespace Foundation::Core {

// Init from argv[0] — call as the very first line of main(), before any chdir().
void PathsInit(const char* argv0);

// Init from a directory path directly (e.g. SDL_GetBasePath()).
void PathsInitFromDir(const char* exeDir);

// Prepend exe-dir to a relative path. Absolute paths are returned unchanged.
// On platforms that register an asset loader (e.g. Android APK assets), each
// relative path is (re)materialized from the bundle into the resolved location
// on every call, so callers can keep using plain fopen/ifstream on the returned
// concrete path and the on-disk copy stays fresh across package updates. Paths
// not present in the bundle (e.g. the writable pipeline-cache path) pass through.
String PathsResolve(StringView relPath);

// Optional loader for platforms that bundle files inside a package (Android APK
// assets). Registered at startup by the SDL/platform bootstrap. Returns a
// malloc'd buffer (freed by the caller via free()) and its size via outSize, or
// nullptr if the bundle has no such entry.
using PathsAssetLoader = void* (*)(const char* relPath, size_t* outSize);
void PathsRegisterAssetLoader(PathsAssetLoader loader);

} // namespace Foundation::Core
