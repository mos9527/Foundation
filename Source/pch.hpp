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

#include <glog/logging.h>
#include <DirectXMath.h>
#include <directxtk/SimpleMath.h>

#define RHI_USE_D3D12
#define RHI_D3D12_USE_AGILITY

#ifdef RHI_USE_D3D12
#include "Renderer/RHI/D3D12/D3D12Pch.h"
#endif
