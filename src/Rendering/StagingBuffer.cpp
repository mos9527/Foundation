#include "StagingBuffer.hpp"
#include "Bits/Format.hpp"

namespace Foundation::Rendering {
    StagingBuffer::StagingBuffer(Allocator* allocator, RHIDevice* device, const size_t size)
        : m_allocator(allocator), m_device(device),
            m_buffer(device->CreateBuffer({.resource = {.heap = RHIDeviceHeapType::Upload, .host_access = RHIResourceHostAccess::WriteOnly},
                                            .usage    = RHIBufferUsageBits::TransferSource,
                                            .size     = size})),
            m_size(size) {
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
    void StagingBuffer::Reset()
    {
        m_offset = 0;
    }

    DataBuffer::DataBuffer(Allocator* allocator, RHIDevice* device, size_t budget, RHIBufferUsageBits usage)
        : m_allocator(allocator), m_allocations(allocator), m_staging(allocator), m_coalescedStaging(allocator)
    {
        m_buffer = device->CreateBuffer({
            .resource =
                {
                    .heap = RHIDeviceHeapType::Local,
                    .host_access = RHIResourceHostAccess::Invisible,
                },
            .usage = usage | RHIBufferUsageBits::TransferDestination,
            .size = budget,
        });
    }

    DataHandle DataBuffer::PushData(StagingBuffer* buffer, Span<const char> data, size_t alignment) {
        auto& [handle, alloc] = m_allocations.pop();
        alloc = m_buffer->GetArena().Allocate(data.size(), alignment);
        if (alloc == kInvalidHandle)
            throw std::bad_alloc();

        auto stagingOffset = buffer->Write(data, alignment);
        CHECK(stagingOffset != ~0ull && "Staging buffer overflow");

        m_staging.emplace_back(stagingOffset, m_buffer->GetArena().GetOffset(alloc), data.size());
        return handle;
    }
    void DataBuffer::UpdateData(StagingBuffer* buffer, DataHandle handle, Span<const char> data, size_t alignment) {
        auto& alloc = m_allocations.at(handle);
        auto size = m_buffer->GetArena().GetSize(handle);
        CHECK(data.size() <= size && "New data cannot be larger than initial allocation");

        auto stagingOffset = buffer->Write(data, alignment);
        CHECK(stagingOffset != ~0ull && "Staging buffer overflow");

        m_staging.emplace_back(stagingOffset, m_buffer->GetArena().GetOffset(handle), data.size());
    }

    void DataBuffer::FreeData(DataHandle handle)
    {
        auto& alloc = m_allocations.at(handle);
        m_buffer->GetArena().Free(alloc);
        m_allocations.free(handle);
    }

    Pair<size_t, size_t> DataBuffer::Query(DataHandle handle) const
    {
        auto& alloc = m_allocations.at(handle);
        return { m_buffer->GetArena().GetSize(alloc), m_buffer->GetArena().GetOffset(alloc) };
    }

    void DataBuffer::Record(RHICommandList* cmd, StagingBuffer* buffer)
    {
        if (m_staging.empty())
            return;
        cmd->CopyBuffer(
            buffer->GetBuffer().Get(),
            m_buffer.Get(),
            CoalesceStaging()
        );
        m_staging.clear();
    }

    void DataBuffer::Abort() {
        m_staging.clear();
        m_staging.shrink_to_fit();
    }

    void DataBuffer::Reset() {
        Abort();
        m_allocations.clear();
        m_buffer->GetArena().Reset();
    }

    DataBuffer::StagingList& DataBuffer::CoalesceStaging()
    {
        m_coalescedStaging.clear();
        if (m_staging.size() <= 1)
            return m_staging;
        std::ranges::sort(m_staging, [](auto const& a, auto const& b) {
            return Pair<size_t,size_t>{a.dst_offset, a.src_offset} < Pair<size_t,size_t>{b.dst_offset, b.src_offset};
        });
        m_staging.erase(std::ranges::unique(m_staging, [](auto const& a, auto const& b)
        {
            return a.dst_offset == b.dst_offset;
        }).begin(), m_staging.end());
        m_coalescedStaging.push_back(m_staging[0]);
        for (size_t i = 1; i < m_staging.size(); ++i)
        {
            size_t d_src = m_staging[i].src_offset - m_staging[i - 1].src_offset;
            size_t d_dst = m_staging[i].dst_offset - m_staging[i - 1].dst_offset;
            size_t sz = m_staging[i].size;
            if (d_src == sz && d_dst == sz)
                m_coalescedStaging.back().size += sz;
            else
                m_coalescedStaging.push_back(m_staging[i]);
        }
        return m_coalescedStaging;
    }

}
