#pragma once
#pragma once
#include "D3D12Types.hpp"
namespace RHI {
	class PipelineState : public DeviceChild {
		name_t m_Name;
		ComPtr<ID3D12PipelineState> m_PipelineState;

	public:
		PipelineState(Device* device, ComPtr<ID3D12PipelineState>&& state) : DeviceChild(device) { m_PipelineState = state; } /* TODO */
		// xxx custom ctor for abstracted RHI pipeline state type
		inline auto GetNativePipelineState() { return m_PipelineState.Get(); }
		inline operator ID3D12PipelineState* () { return m_PipelineState.Get(); }

		inline auto const& GetName() { return m_Name; }
		inline void SetName(name_t name) {
			m_Name = name;
			m_PipelineState->SetName(name);
		}
	};
}