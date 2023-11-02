#pragma once
#include "Common.hpp"

#ifdef RHI_USE_D3D12
#include "D3D12/D3D12Device.hpp"
#include "D3D12/D3D12Swapchain.hpp"
#include "D3D12/D3D12CommandQueue.hpp"
#include "D3D12/D3D12CommandList.hpp"
#include "D3D12/D3D12Buffer.hpp"
#include "D3D12/D3D12Texture.hpp"
#include "D3D12/D3D12DescriptorHeap.hpp"
#include "D3D12/D3D12Types.hpp"
#include "D3D12/D3D12Shader.hpp"
#endif