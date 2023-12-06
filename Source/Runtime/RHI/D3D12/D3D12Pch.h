#pragma once
#pragma comment(lib,"dxgi.lib")
#pragma comment(lib,"d3d12.lib")
#pragma comment(lib,"d3dcompiler.lib")
#pragma comment(lib,"dxcompiler.lib")
#pragma comment(lib,"WinPixEventRuntime.lib")

#include <directx/d3dx12.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <dxcapi.h>
#define USE_PIX
#include <pix3.h>

#define D3D12MA_D3D12_HEADERS_ALREADY_INCLUDED
#include "D3D12MemAlloc.h"

#define D3D_MIN_FEATURE_LEVEL D3D_FEATURE_LEVEL_12_1

#ifdef TRACY_ENABLE
#include "../../../../Dependencies/tracy/public/tracy/TracyD3D12.hpp"
#endif

using Microsoft::WRL::ComPtr;
using namespace DirectX;
using namespace SimpleMath;
namespace RHI {
	typedef const wchar_t* name_t;
}
// HLSL typenames
typedef UINT uint;
typedef XMUINT2 uint2;
typedef XMUINT3 uint3;
typedef XMUINT4 uint4;
typedef Vector2 float2;
typedef Vector3 float3;
typedef Vector4 float4;
typedef Matrix matrix; // row-major in XMMATH, col-major in HLSL (by default)
