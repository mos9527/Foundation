#pragma once
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

#define WIN32_LEAN_AND_MEAN
#define NOGDI
#include <Windows.h>
#include <wrl.h>

#include <glog/logging.h>
#include <DirectXMath.h>

#define RHI_USE_D3D12
#define RHI_D3D12_USE_AGILITY

#ifdef RHI_USE_D3D12
#include "Renderer/RHI/D3D12/D3D12Pch.h"
#endif
