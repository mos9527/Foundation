#include "Bits/Format.hpp"
#include "Scene.hpp"
namespace Foundation {
    SceneData::SceneData(Allocator* allocator, RHIDevice* device, SceneDataDesc const& desc) :
        m_allocator(allocator), m_staging(allocator), m_primitives(allocator), m_vertices(allocator), m_indices(allocator), m_textures(allocator),
        m_instances(allocator), m_primitiveStaging(allocator), m_instanceStaging(allocator), m_vertexStaging(allocator),
        m_indexStaging(allocator), m_desc(desc), m_waitIdle(device)
    {
        LOG_RUNTIME(SceneData, info, "** Scene Data Budgets **");
        LOG_RUNTIME(SceneData, info, "  Primitive Data: {}", formatHumanReadableSize(desc.PrimitiveDataBudget));
        LOG_RUNTIME(SceneData, info, "  Instance Data:  {}", formatHumanReadableSize(desc.InstanceDataBudget));
        LOG_RUNTIME(SceneData, info, "  Vertex Data:    {}", formatHumanReadableSize(desc.VertexDataBudget));
        LOG_RUNTIME(SceneData, info, "  Index Data:     {}", formatHumanReadableSize(desc.IndexDataBudget));
        m_primitiveData = device->CreateBuffer({
            .resource =
                {
                    .heap = RHIDeviceHeapType::Local,
                    .host_access = RHIResourceHostAccess::Invisible,
                },
            .usage =
                RHIBufferUsageBits::VertexBuffer | RHIBufferUsageBits::IndexBuffer | RHIBufferUsageBits::StorageBuffer | RHIBufferUsageBits::TransferDestination,
            .size = desc.PrimitiveDataBudget,
        });
        m_instanceData = device->CreateBuffer({
            .resource =
                {
                    .heap = RHIDeviceHeapType::Local,
                    .host_access = RHIResourceHostAccess::Invisible,
                },
            .usage = RHIBufferUsageBits::StorageBuffer | RHIBufferUsageBits::TransferDestination,
            .size = desc.InstanceDataBudget,
        });
        m_vertexData = device->CreateBuffer({
            .resource =
                {
                    .heap = RHIDeviceHeapType::Local,
                    .host_access = RHIResourceHostAccess::Invisible,
                },
            .usage = RHIBufferUsageBits::VertexBuffer | RHIBufferUsageBits::TransferDestination,
            .size = desc.InstanceDataBudget,
        });
        m_indexData = device->CreateBuffer({
            .resource =
                {
                    .heap = RHIDeviceHeapType::Local,
                    .host_access = RHIResourceHostAccess::Invisible,
                },
            .usage = RHIBufferUsageBits::IndexBuffer | RHIBufferUsageBits::TransferDestination,
            .size = desc.IndexDataBudget,
        });
        m_stagingBuffer = device->CreateBuffer({
            .resource =
                {
                    .heap = RHIDeviceHeapType::Upload,
                    .host_access = RHIResourceHostAccess::WriteOnly,
                },
                .usage = RHIBufferUsageBits::TransferSource,
                .size = desc.TotalBudget()
        });
    }
    RHIBuffer::Arena::Allocation SceneData::PushData(RHIBuffer* buffer, StagingList& staging, Span<const char> data, size_t alignment)
    {
        auto alloc = buffer->GetArena().Allocate(data.size(), alignment);
        if (alloc == kInvalidHandle)
            throw std::bad_alloc();
        staging.emplace_back(m_staging.size(), buffer->GetArena().GetOffset(alloc), data.size());
        m_staging.insert(m_staging.end(), data.begin(), data.end());
        return alloc;
    }
    void SceneData::UpdateData(RHIBuffer* buffer, StagingList& staging, RHIBuffer::Arena::Allocation handle, Span<const char> data)
    {
        auto size = buffer->GetArena().GetSize(handle);
        CHECK(data.size() <= size && "New data cannot be larger than initial allocation");
        staging.emplace_back(m_staging.size(), buffer->GetArena().GetOffset(handle), data.size());
        m_staging.insert(m_staging.end(), data.begin(), data.end());
    }
    void SceneData::FreeData(RHIBuffer* buffer, RHIBuffer::Arena::Allocation alloc) {
        buffer->GetArena().Free(alloc);
    }
    /**
     * NOTE: This should only be called within Scene::Update, after checking HasUpdates()
     * and within the Scene Update Pass that it creates.
     */
    void SceneData::EndTransfer(RHICommandList* cmd)
    {
        std::memcpy(m_stagingBuffer->Map(), m_staging.data(), m_staging.size());
        m_stagingBuffer->Flush(), m_staging.clear(), m_staging.shrink_to_fit();
        using enum RHIResourceAccessBits;
        using enum RHIPipelineStageBits;
        // Transfers
        if (!m_primitiveStaging.empty())
            cmd->CopyBuffer(m_stagingBuffer.Get(), m_primitiveData.Get(), m_primitiveStaging);
        if (!m_instanceStaging.empty())
            cmd->CopyBuffer(m_stagingBuffer.Get(), m_instanceData.Get(), m_instanceStaging);
        if (!m_vertexStaging.empty())
            cmd->CopyBuffer(m_stagingBuffer.Get(), m_vertexData.Get(), m_vertexStaging);
        if (!m_indexStaging.empty())
            cmd->CopyBuffer(m_stagingBuffer.Get(), m_indexData.Get(), m_indexStaging);
        m_primitiveStaging.clear(), m_instanceStaging.clear(), m_vertexStaging.clear(), m_indexStaging.clear();
    }
    void SceneData::Update(RHICommandList* cmd) {
        // Initial clear on instance data
        if (!m_initialized)
        {
            cmd->FillBuffer(m_instanceData.Get(), 0);
            m_initialized = true;
        }
        if (!m_staging.empty())
            EndTransfer(cmd);
    }

    SceneHandle SceneData::AddData(Span<const char> data, RHIBuffer* buffer, AllocationList& alist, StagingList& staging, size_t alignment) {
        auto& [handle, alloc] = alist.pop();
        alloc = PushData(buffer, staging, data, alignment);
        return handle;
    }
    void SceneData::UpdateData(SceneHandle handle, Span<const char> data, RHIBuffer* buffer, AllocationList& alist, StagingList& staging) {
        auto& alloc = alist.at(handle);
        UpdateData(buffer, staging, alloc, data);
    }
    void SceneData::FreeData(SceneHandle handle, RHIBuffer* buffer, AllocationList& alist)
    {
        auto& alloc = alist.at(handle);
        FreeData(buffer, alloc);
        alist.free(handle);
    }
    Pair<size_t, size_t> SceneData::QueryDataSizeAndOffset(SceneHandle handle, RHIBuffer* buffer, AllocationList const& alist) const
    {
        auto& alloc = alist.at(handle);
        return { buffer->GetArena().GetSize(alloc), buffer->GetArena().GetOffset(alloc) };
    }

    void SceneData::Abort() {
        m_primitiveStaging.clear(), m_primitiveStaging.shrink_to_fit();
        m_instanceStaging.clear(), m_instanceStaging.shrink_to_fit();
        m_vertexStaging.clear(), m_vertexStaging.shrink_to_fit();
        m_indexStaging.clear(), m_indexStaging.shrink_to_fit();
    }

    void SceneData::Reset() {
        Abort();
        m_primitives.clear();
        m_instances.clear();
        m_primitiveData->GetArena().Reset();
        m_instanceData->GetArena().Reset();
        m_vertexData->GetArena().Reset();
        m_indexData->GetArena().Reset();
    }
}
