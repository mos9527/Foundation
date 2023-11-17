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
#include <pix3.h>

#define D3D12MA_D3D12_HEADERS_ALREADY_INCLUDED
#include "D3D12MemAlloc.h"

#define D3D_MIN_FEATURE_LEVEL D3D_FEATURE_LEVEL_12_1

using namespace DirectX;
using Microsoft::WRL::ComPtr;

namespace RHI {
	typedef const wchar_t* name_t;
}

typedef uint32_t uint;