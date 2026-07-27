#pragma once

#include <cctype>
#include <Core/Container.hpp>

// Lexical path helpers; no filesystem access. Separators may be '/' or '\\'.
// These never touch the OS, so they are safe to call from noexcept contexts.
namespace EditorPathUtil
{
inline Foundation::Core::StringView ParentPath(Foundation::Core::StringView path)
{
    auto pos = path.find_last_of("/\\");
    return pos == Foundation::Core::StringView::npos ? Foundation::Core::StringView{} : path.substr(0, pos);
}

inline Foundation::Core::StringView FileName(Foundation::Core::StringView path)
{
    auto pos = path.find_last_of("/\\");
    return pos == Foundation::Core::StringView::npos ? path : path.substr(pos + 1);
}

inline Foundation::Core::StringView Stem(Foundation::Core::StringView path)
{
    Foundation::Core::StringView name = FileName(path);
    auto dot = name.find_last_of('.');
    return (dot == Foundation::Core::StringView::npos || dot == 0) ? name : name.substr(0, dot);
}

inline Foundation::Core::String JoinPath(Foundation::Core::StringView dir, Foundation::Core::StringView file)
{
    if (dir.empty())
        return Foundation::Core::String(file);
    Foundation::Core::String out(dir);
    if (out.back() != '/' && out.back() != '\\')
        out += '/';
    out += file;
    return out;
}

// Lowercase file extension including the leading '.', or empty if none.
// Only the final path component is considered (a dot in a directory segment is ignored).
inline Foundation::Core::String LowerExtension(Foundation::Core::StringView path)
{
    Foundation::Core::StringView name = FileName(path);
    auto dot = name.find_last_of('.');
    Foundation::Core::String ext = (dot == Foundation::Core::StringView::npos || dot == 0)
                                       ? Foundation::Core::String{}
                                       : Foundation::Core::String(name.substr(dot));
    for (char& c : ext)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext;
}
} // namespace EditorPathUtil
