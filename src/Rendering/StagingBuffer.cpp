#include "StagingBuffer.hpp"
#include "Bits/Format.hpp"

namespace Foundation::Rendering
{
    BufferStagingList& CoalesceBufferStaging(BufferStagingList& res)
    {
        if (res.size() <= 1)
            return res;
        auto SortKey = [](BufferStagingItem const& item)
        {
            auto const& [src, dst, region] = item;
            return Tuple{reinterpret_cast<size_t>(src), reinterpret_cast<size_t>(dst), region.dst_offset,
                         region.src_offset};
        };
        Ranges::sort(res, [SortKey](BufferStagingItem& a, BufferStagingItem& b) { return SortKey(a) < SortKey(b); });
        res.erase(Ranges::unique(res, [SortKey](BufferStagingItem& a, BufferStagingItem& b)
                                 { return SortKey(a) == SortKey(b); })
                      .begin(),
                  res.end());
        Span<BufferStagingItem> staging = {res.begin(), res.end()};
        size_t i = 1, j = 0;
        for (; i < staging.size(); ++i)
        {
            auto const& [src, dst, region] = staging[i];
            auto const& [prev_src, prev_dst, prev_region] = staging[i - 1];
            size_t d_src = region.src_offset - prev_region.src_offset;
            size_t d_dst = region.dst_offset - prev_region.dst_offset;
            size_t sz = region.size;
            auto& [top_src, top_dst, top_region] = res[j];
            if (src == prev_src && dst == prev_dst && d_src == sz && d_dst == sz)
                top_region.size += sz;
            else
                j++, res[j] = staging[i];
        }
        res.resize(j + 1);
        return res;
    }
    StagingBuffer::StagingBuffer(Allocator* allocator, RHIDevice* device, const size_t size) :
        m_allocator(allocator), m_device(device),
        m_buffer(device->CreateBuffer(
            {.resource = {.heap = RHIDeviceHeapType::Upload, .host_access = RHIResourceHostAccess::WriteOnly},
             .usage = RHIBufferUsageBits::TransferSource,
             .size = size})),
        m_size(size)
    {
        m_mapped = m_buffer->Map();
    }
    size_t StagingBuffer::Write(const Span<const char> data, const size_t alignment)
    {
        size_t alignedOffset = (m_offset + alignment - 1) & ~(alignment - 1);
        CHECK_MSG(alignedOffset + data.size() < m_size, "Staging buffer overflow");
        std::memcpy(static_cast<char*>(m_mapped) + alignedOffset, data.data(), data.size());
        m_offset = alignedOffset + data.size();
        return alignedOffset;
    }
    void StagingBuffer::Seek(size_t offset, const size_t alignment)
    {
        size_t alignedOffset = (m_offset + alignment - 1) & ~(alignment - 1);
        m_offset = alignedOffset + offset;
        CHECK_MSG(m_offset < m_size, "Staging buffer overflow");
    }
    void StagingBuffer::Reset() { m_offset = 0; }

    Staging::Staging(RHIDevice* device, Allocator* allocator, uint32_t numSwaps, size_t stagingBudget) :
        m_device(device), m_allocator(allocator), m_stagingBuffers(allocator), m_bufferStagings(allocator),
        m_idleGuard(m_device)
    {
        // Create one staging buffer for each frame in flight.
        // This in turn utilizes the frame fences for synchronization.
        // A lower memory footprint option is to do this with a single staging buffer,
        // and wait for the resource fence before reusing it, which
        // incurs CPU stalls.
        for (size_t i = 0; i < numSwaps; i++)
        {
            m_stagingBuffers.emplace_back(allocator, device, stagingBudget);
            m_stagingBuffers.back().GetBuffer()->DebugSetObjectName(fmt::format("Staging Buffer {}", i).c_str());
        }
        LOG_RUNTIME(Staging, info, "** Staging init with {} buffers of {} each **", numSwaps,
                    formatHumanReadableSize(stagingBudget));
    }
    void Staging::BeginTransfer(uint32_t rendererSync)
    {
        CHECK_MSG(m_state == State::Idle, "Staging is already in {} state", m_state);
        m_currentSync = rendererSync;
        m_stagingBuffers[m_currentSync].Reset();
        m_bufferStagings.clear();
        m_state = State::Transfer;
    }
    RHIBuffer* Staging::GetStagingBuffer()
    {
        CHECK_MSG(m_state == State::Transfer, "Staging is not in Transfer state");
        CHECK_MSG(m_currentSync < m_stagingBuffers.size(), "Invalid current sync index {}", m_currentSync);
        return m_stagingBuffers[m_currentSync].GetBuffer();
    }
    void Staging::TransferBuffer(RHIBuffer* dst_buffer, size_t offset, Span<const char> data, size_t alignment)
    {
        CHECK_MSG(m_state == State::Transfer, "Staging is not in Transfer state");
        size_t src_offset = m_stagingBuffers[m_currentSync].Write(data, alignment);
        m_bufferStagings.emplace_back(m_stagingBuffers[m_currentSync].GetBuffer(), dst_buffer,
                                      RHICommandList::CopyBufferRegion{src_offset, offset, data.size_bytes()});
    }
    void Staging::EndTransfer()
    {
        CHECK_MSG(m_state == State::Transfer, "Staging is not in Transfer state");
        CoalesceBufferStaging(m_bufferStagings);
        m_state = State::Idle;
        m_currentSync = ~0u;
    }
} // namespace Foundation::Rendering
