#pragma once
#pragma once
#include "../Common.hpp"
namespace RHI {
	class PipelineState : public RHIObject {
		using RHIObject::m_Name;
		ComPtr<ID3D12PipelineState> m_PipelineState;

	public:
		PipelineState(ComPtr<ID3D12PipelineState>&& state) { m_PipelineState = state; } /* TODO */

		inline auto GetNativePipelineState() { return m_PipelineState.Get(); }
		inline operator ID3D12PipelineState* () { return m_PipelineState.Get(); }

		using RHIObject::GetName;
		inline void SetName(name_t name) {
			m_Name = name;
			m_PipelineState->SetName((const wchar_t*)name.c_str());
		}
	};
}