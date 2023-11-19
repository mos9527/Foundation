#pragma once
#pragma warning( disable : 4267 ) // Ignore size-mismatch errors
#pragma warning( disable : 4244 )

#include <ctype.h>
#include <vector>
#include <array>
#include <algorithm>
#include <cassert>
#include <string>
#include <stdexcept>
#include <memory>
#include <queue>
#include <unordered_map>
#include <concepts>
#include <numeric>
#include <filesystem>
#include <thread>
#include <future>
#include <stack>
#include <fstream>

#define WIN32_LEAN_AND_MEAN
#define NOGDI
#define NOMINMAX
#include <Windows.h>
#include <wrl.h>

#define ENTT_ID_TYPE uint32_t
#include "../Dependencies/entt/single_include/entt/entt.hpp"

#include <glog/logging.h>
#include <DirectXMath.h>
#include <directxtk/SimpleMath.h>

#include "defines.hpp"

#ifdef RHI_USE_D3D12
#include "Runtime/RHI/D3D12/D3D12Pch.h"
#endif

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_WINDOWS_UTF8
#include "../Dependencies/stb_image.h"

#ifdef IMGUI_ENABLED
#define IMGUI_DEFINE_MATH_OPERATORS
#include "../../Dependencies/imgui/imgui.h"
#include "../../Dependencies/imgui/backends/imgui_impl_dx12.h"
#include "../../Dependencies/imgui/backends/imgui_impl_win32.h"
#endif

#include "Common/Helpers.hpp"

// HLSL typenames
typedef UINT uint;
typedef XMUINT2 uint2;
typedef XMUINT3 uint3;
typedef XMUINT4 uint4;
typedef SimpleMath::Vector2 float2;
typedef SimpleMath::Vector3 float3;
typedef SimpleMath::Vector4 float4;
typedef SimpleMath::Matrix matrix; // row-major in XMMATH, col-major in HLSL
// Straddles elements to Base boundaries
template<typename Base, typename ToPad> struct padded_elem {
    Base data{};
    void operator=(ToPad value) { memcpy(&data, &value, sizeof(value)); }
};
typedef padded_elem<uint4, uint> padded_uint;
typedef padded_elem<uint4, uint2> padded_uint2;
typedef padded_elem<uint4, uint3> padded_uint3;
typedef padded_elem<float4, float> padded_float;
typedef padded_elem<float4, float2> padded_float2;
typedef padded_elem<float4, float3> padded_float3;