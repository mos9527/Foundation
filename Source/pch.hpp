#pragma once
#pragma warning( disable : 4267 ) // Ignore size-mismatch errors
#pragma warning( disable : 4244 )

#include <ctype.h>
#include <vector>
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
#include "../Dependencies/entt/entt.hpp"

#include <glog/logging.h>
#include <DirectXMath.h>
#include <directxtk/SimpleMath.h>

#include "defines.hpp"

#ifdef RHI_USE_D3D12
#include "Renderer/RHI/D3D12/D3D12Pch.h"
#endif

#include "Common/Helpers.hpp"

// HLSL typenames
typedef UINT uint;
typedef XMUINT2 uint2;
typedef XMUINT3 uint3;
typedef XMUINT4 uint4;
typedef XMFLOAT2 float2;
typedef XMFLOAT3 float3;
typedef XMFLOAT4 float4;
typedef XMMATRIX matrix;
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