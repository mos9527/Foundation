#include "Bits/Format.hpp"
#include "Staging.hpp"

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

    StagedBuffer::StagedBuffer(RHIDevice* device, Allocator* allocator, uint32_t numSwaps, RHIBufferDesc const& desc,
                               size_t stagingBudget, Optional<uint32_t> clearValue) :
        m_device(device), m_allocator(allocator), m_stagingBuffers(allocator), m_bufferStagings(allocator),
        m_idleGuard(m_device), m_clearValue(clearValue)
    {
        // Create one staging buffer for each frame in flight.
        // This in turn utilizes the frame fences for synchronization.
        // A lower memory footprint option is to do this with a single staging buffer,
        // and wait for the resource fence before reusing it, which
        // incurs CPU stalls.
        if (stagingBudget == kFullSize)
            stagingBudget = desc.size;
        for (size_t i = 0; i < numSwaps; i++)
        {
            m_stagingBuffers.emplace_back(allocator, device, stagingBudget);
            m_stagingBuffers.back().GetBuffer()->DebugSetObjectName(fmt::format("Staging Buffer {}", i).c_str());
        }
        m_buffer = device->CreateBuffer(desc);
        LOG_RUNTIME(Staging, info, "** Staging init with {} buffers of {} each **", numSwaps,
                    formatHumanReadableSize(desc.size));
    }
    void StagedBuffer::BeginTransfer(uint32_t rendererSync)
    {
        CHECK_MSG(m_state == State::Idle, "Staging is already in {} state", m_state);
        m_currentSync = rendererSync;
        m_stagingBuffers[m_currentSync].Reset();
        m_bufferStagings.clear();
        m_state = State::Transfer;
    }
    StagingBuffer* StagedBuffer::GetStagingBuffer()
    {
        CHECK_MSG(m_state == State::Transfer, "Staging is not in Transfer state");
        CHECK_MSG(m_currentSync < m_stagingBuffers.size(), "Invalid current sync index {}", m_currentSync);
        return &m_stagingBuffers[m_currentSync];
    }
    void StagedBuffer::Transfer(size_t dst_offset, Span<const char> data, size_t alignment)
    {
        CHECK_MSG(m_state == State::Transfer, "Staging is not in Transfer state");
        size_t src_offset = m_stagingBuffers[m_currentSync].Write(data, alignment);
        if (!m_bufferStagings.empty())
        {
            // Check if we can coalesce the buffer transfer right away
            auto& [last_src, last_dst, last_region] = m_bufferStagings.back();
            if (last_src == m_stagingBuffers[m_currentSync].GetBuffer() && last_dst == m_buffer.Get() &&
                dst_offset - last_region.dst_offset == last_region.size &&
                src_offset - last_region.src_offset == last_region.size)
            {
                last_region.size += data.size_bytes();
                return;
            }
        }
        m_bufferStagings.emplace_back(m_stagingBuffers[m_currentSync].GetBuffer(), m_buffer.Get(),
                                      RHICommandList::CopyBufferRegion{src_offset, dst_offset, data.size_bytes()});
    }
    BufferAllocation StagedBuffer::Push(Span<const char> data, size_t alignment)
    {
        CHECK_MSG(m_state == State::Transfer, "Staging is not in Transfer state");
        auto& arena = m_buffer->GetArena();
        BufferAllocation alloc = arena.Allocate(data.size(), alignment);
        auto [size, offset] = Query(alloc);
        Transfer(offset, data, alignment);
        return alloc;
    }
    void StagedBuffer::Emplace(BufferAllocation, Span<const char> data, size_t alignment)
    {
        CHECK_MSG(m_state == State::Transfer, "Staging is not in Transfer state");
        auto& arena = m_buffer->GetArena();
        BufferAllocation alloc = arena.Allocate(data.size(), alignment);
        auto [size, offset] = Query(alloc);
        Transfer(offset, data, alignment);
    }
    AllocationPair StagedBuffer::Query(BufferAllocation allocation) const
    {
        CHECK_MSG(m_state == State::Transfer, "Staging is not in Transfer state");
        auto& arena = m_buffer->GetArena();
        return {arena.GetSize(allocation), arena.GetOffset(allocation)};
    }
    void StagedBuffer::Pop(BufferAllocation allocation)
    {
        auto& arena = m_buffer->GetArena();
        arena.Free(allocation);
    }
    void StagedBuffer::EndTransfer()
    {
        CHECK_MSG(m_state == State::Transfer, "Staging is not in Transfer state");
        CoalesceBufferStaging(m_bufferStagings);
        m_state = State::Idle;
        m_currentSync = ~0u;
    }
    void StagedBuffer::Update(RHICommandList* cmd)
    {
        CHECK_MSG(m_state == State::Idle, "Staging is not in Idle state");
        if (!HasUpdates())
            return;
        cmd->DebugBegin("Staging Buffer Updates");
        if (m_clearValue.has_value())
            cmd->FillBuffer(m_buffer.Get(), m_clearValue.value()), m_clearValue.reset();
        for (auto const& [src, dst, range] : m_bufferStagings)
            cmd->CopyBuffer(src, dst, {range});
        cmd->DebugEnd();
        m_bufferStagings.clear();
    }
} // namespace Foundation::Rendering
