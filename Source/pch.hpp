#pragma once
#pragma warning( disable : 4267 ) // Ignore size-mismatch errors
#pragma warning( disable : 4244 )
#pragma warning( disable : 4305 )

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
#include <variant>
#include <execution>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <Windows.h>
#include <wrl.h>
#include <windowsx.h>
#include <hidusage.h>
#include <commdlg.h>
#include <wingdi.h>
#undef ERROR // Hack for GLOG's abbervatived error levels. GDI defines this macro.

#define ENTT_ID_TYPE uint32_t
#include "entt/entt.hpp"

#include <glog/logging.h>
#include <DirectXMath.h>
#include <directxtk/SimpleMath.h>

#include "Common/Defines.hpp"

#ifdef TRACY_ENABLE
#include "tracy/Tracy.hpp"
#endif

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
#include "../../Dependencies/imgui/imgui_internal.h"
#include "../../Dependencies/imgui/backends/imgui_impl_dx12.h"
#include "../../Dependencies/imgui/backends/imgui_impl_win32.h"
#endif

#include <assimp/Importer.hpp>      // C++ importer interface
#include <assimp/scene.h>           // Output data structure
#include <assimp/postprocess.h>
#include "assimp/Logger.hpp"
#include "assimp/DefaultLogger.hpp"
#include "assimp/LogStream.hpp"
#include <meshoptimizer.h>

#include "Common/Defines.hpp"
#include "Common/Helpers.hpp"
