#pragma once
#include <Core/Container.hpp>
using namespace Foundation::Core;

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>

/**
 * @brief 打开Win32原生文件选择对话框
 * @param filter 文件过滤器，例如 L"Scene Files\0*.gltf;*.glb;*.fscn\0All Files\0*.*\0"
 * @param title 对话框标题
 * @return 选中的文件路径，取消返回 nullopt
 */
inline Optional<String> OpenFileDialog(const wchar_t* filter, const wchar_t* title)
{
    wchar_t filename[MAX_PATH] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = nullptr;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;

    if (GetOpenFileNameW(&ofn))
    {
        // 将 wchar_t 转为 UTF-8 string
        int size = WideCharToMultiByte(CP_UTF8, 0, filename, -1, nullptr, 0, nullptr, nullptr);
        String result(size - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, filename, -1, result.data(), size, nullptr, nullptr);
        return result;
    }
    return std::nullopt;
}

/**
 * @brief 打开Win32原生文件保存对话框
 * @param filter 文件过滤器
 * @param title 对话框标题
 * @param defaultExt 默认扩展名，例如 L"fscn"
 * @return 保存路径，取消返回 nullopt
 */
inline Optional<String> SaveFileDialog(const wchar_t* filter, const wchar_t* title, const wchar_t* defaultExt)
{
    wchar_t filename[MAX_PATH] = {};
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = nullptr;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = filename;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = title;
    ofn.lpstrDefExt = defaultExt;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;

    if (GetSaveFileNameW(&ofn))
    {
        int size = WideCharToMultiByte(CP_UTF8, 0, filename, -1, nullptr, 0, nullptr, nullptr);
        String result(size - 1, '\0');
        WideCharToMultiByte(CP_UTF8, 0, filename, -1, result.data(), size, nullptr, nullptr);
        return result;
    }
    return std::nullopt;
}

#else
// 非Windows平台暂不支持文件对话框
inline Optional<String> OpenFileDialog(const wchar_t*, const wchar_t*) { return std::nullopt; }
inline Optional<String> SaveFileDialog(const wchar_t*, const wchar_t*, const wchar_t*) { return std::nullopt; }
#endif
