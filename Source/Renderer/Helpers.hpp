#pragma once
#include "../pch.hpp"
static std::wstring utf8_to_wstring(const char* src)
{
	const int size_needed = MultiByteToWideChar(CP_UTF8, 0, src, (int)strlen(src), NULL, 0);
	if (size_needed <= 0)
	{
		return L"";
	}

	std::wstring result(size_needed, 0);
	MultiByteToWideChar(CP_UTF8, 0, src, (int)strlen(src), &result.at(0), size_needed);
	return result;
}

static std::string wstring_to_utf8(const wchar_t* src)
{
	const int size_needed = WideCharToMultiByte(CP_UTF8, 0, src, (int)wcslen(src), NULL, 0, NULL, NULL);
	if (size_needed <= 0)
	{
		return "";
	}
	std::string result(size_needed, 0);
	WideCharToMultiByte(CP_UTF8, 0, src, (int)wcslen(src), &result.at(0), size_needed, NULL, NULL);
	return result;
}
static std::string size_to_str(size_t size)
{
    if (size == 0)
        return "0";
    char result[32];
    double size2 = (double)size;
    if (size2 >= 1024.0 * 1024.0 * 1024.0 * 1024.0)
    {
        sprintf_s(result, "%.2f TB", size2 / (1024.0 * 1024.0 * 1024.0 * 1024.0));
    }
    else if (size2 >= 1024.0 * 1024.0 * 1024.0)
    {
        sprintf_s(result, "%.2f GB", size2 / (1024.0 * 1024.0 * 1024.0));
    }
    else if (size2 >= 1024.0 * 1024.0)
    {
        sprintf_s(result, "%.2f MB", size2 / (1024.0 * 1024.0));
    }
    else if (size2 >= 1024.0)
    {
        sprintf_s(result, "%.2f KB", size2 / 1024.0);
    }
    else
        sprintf_s(result, "%llu B", size);
    return result;
}
#define CHECK_ENUM_FLAG(x) { CHECK((size_t)(x) > 0); }
#define CHECK_HR(hr) { HRESULT _result = hr; if (FAILED(_result)) { LOG_SYSRESULT(_result); LOG(FATAL) << "FATAL APPLICATION ERROR HRESULT 0x" << std::hex << _result;  abort(); } }
#ifdef _DEBUG
#define DCHECK_HR CHECK_HR
#define DCHECK_ENUM_FLAG CHECK_ENUM_FLAG
#else 
#define DCHECK_HR(x) {}
#define DCHECK_ENUM_FLAG(x) {}
#endif