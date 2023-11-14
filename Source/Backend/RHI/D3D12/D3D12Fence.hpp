#pragma once
#include "D3D12Types.hpp"
namespace RHI {
	class Device;
	class CommandQueue;
	class Fence : public DeviceChild {
	protected:
		name_t m_Name;
		ComPtr<ID3D12Fence> m_Fence;
		HANDLE m_FenceEvent{ nullptr };
	public:
		Fence(Device* device);
		~Fence();

		inline bool IsCompleted(size_t value) { return GetCompletedValue() == value; }
		inline size_t GetCompletedValue() { return m_Fence->GetCompletedValue(); }
		/* CPU Side fencing */
		inline void Signal(size_t value) { m_Fence->Signal(value); }
		void Wait(size_t value);

		inline operator ID3D12Fence* () { return m_Fence.Get(); }

		inline auto const& GetName() { return m_Name; }
		inline void SetName(name_t name) { 
			m_Name = name; 
			m_Fence->SetName((const wchar_t*)name.c_str()); 
		}
	};

	struct SyncFence {
		Fence* fence;
		size_t value;
		SyncFence(Fence* fence, size_t value) : fence(fence), value(value) {};
		inline bool IsCompleted() { return fence->IsCompleted(value); }
		inline size_t GetValue() { return value; }
		inline void Wait() { fence->Wait(value); }
	};

	template<Releaseable T> class DeferredSyncedReleaseQueue {
		struct Entry {
			std::vector<SyncFence> sync_fences;
			T resource;
			bool all_synced() {
				for (auto& fence : sync_fences) if (!fence.IsCompleted()) return false;
				return true;
			}
			void release() {
				resource.release();
			}
		};
		std::queue<Entry> m_Entries;
	public:
		void push(Entry&& entry) { m_Entries.push(entry); }
		template<typename... Pack> void push(T resource,Pack&&... syncFences) {
			push(Entry{
				.syncFences = { std::forward<Pack>(sync)... },
				.resource = resource
			});
		}
		uint clean() {
			uint cleaned = 0;
			while (!m_Entries.empty()) {
				Entry& entry = m_Entries.front();
				if (!entry.all_synced()) break;
				entry.release();
				m_Entries.pop();
				cleaned++;
			}
			return cleaned;
		}
	};
}