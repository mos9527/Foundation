#pragma once
static inline std::wstring utf8_to_wstring(const char* src)
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

static inline std::string wstring_to_utf8(const wchar_t* src)
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
static inline std::string size_to_str(size_t size)
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
static inline size_t size_in_bytes(auto c) {
    return c.size() * sizeof(decltype(c)::value_type);
}
template<std::integral T> class free_list : private std::vector<T> {
	using Container = std::vector<T>;
public:
	typedef T elem_type;
	free_list() {};
	free_list(T size, size_t initial_value = 0) { reset(size, initial_value); }
	inline void reset(T size, size_t initial_value = 0) {
		Container::resize(size);
		std::iota(Container::rbegin(), Container::rend(), initial_value);
	}
	inline void push(T one) {
		Container::push_back(one);
	}
	inline void push(T* values, size_t count) {
		Container::insert(Container::end(), values, values + count);
	}
	inline void push(free_list<T> const& other) {
		Container::insert(Container::end(), other.begin(), other.end());
	}
	inline T pop() {
		T one = Container::back();
		Container::pop_back();
		return one;
	}
	inline T size() {
		return Container::size();
	}	
};
// From D3D12ExecuteIndirect
template <typename T, typename U>
constexpr T GetAlignedSize(T size, U alignment)
{
	const T alignedSize = (size + alignment - 1) & ~(alignment - 1);
	return alignedSize;
}
// An integer version of ceil(num / denom)
template <typename T, typename U>
constexpr T DivRoundUp(T num, U denom)
{
	return (num + denom - 1) / denom;
}
// We pack the UAV counter into the same buffer as the commands rather than create
// a separate 64K resource/heap for it. The counter must be aligned on 4K boundaries,
// so we pad the command buffer (if necessary) such that the counter will be placed
// at a valid location in the buffer.
constexpr UINT AlignForUavCounter(UINT bufferSize)
{
	return GetAlignedSize(bufferSize, D3D12_UAV_COUNTER_PLACEMENT_ALIGNMENT);	
}
#define CHECK_ENUM_FLAG(x) { CHECK((size_t)(x) > 0); }
#define CHECK_HR(hr) { HRESULT _result = hr; if (FAILED(_result)) { LOG_SYSRESULT(_result); LOG(FATAL) << "FATAL APPLICATION ERROR HRESULT 0x" << std::hex << _result; } }
#ifdef _DEBUG
#define DCHECK_HR CHECK_HR
#define DCHECK_ENUM_FLAG CHECK_ENUM_FLAG
#else 
#define DCHECK_HR(x) {}
#define DCHECK_ENUM_FLAG(x) {}
#endif