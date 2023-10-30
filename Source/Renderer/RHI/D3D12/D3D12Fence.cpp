#include "D3D12Device.hpp"
#include "D3D12Fence.hpp"
namespace RHI {
	Fence::Fence(Device* gpuDevice) {
		auto device = gpuDevice->operator ID3D12Device * ();
		CHECK_HR(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_Fence)));
		m_FenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
		if (m_FenceEvent == nullptr) CHECK_HR(GetLastError());
	}

	Fence::~Fence() {
		if (m_FenceEvent) CloseHandle(m_FenceEvent);
		if (m_Fence) m_Fence.Reset();
	}

	void Fence::Wait(size_t value) {
		if (!IsCompleted(value)) {
			CHECK_HR(m_Fence->SetEventOnCompletion(value, m_FenceEvent));
			WaitForSingleObjectEx(m_FenceEvent, INFINITE, FALSE);
		}
	}
}