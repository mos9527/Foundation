#include "Scene.hpp"
#include "Renderer.hpp"
namespace Foundation {
    SceneData::SceneData(Allocator* allocator, Renderer& renderer, SceneDataDesc const& desc)
        : m_allocator(allocator), m_renderer(renderer), m_desc(desc),
        m_primitives(allocator), m_textures(allocator), m_instances(allocator),
        m_primitiveStaging(allocator), m_instanceStaging(allocator), m_staging(allocator) {
        m_primitiveData = renderer.GetDevice()->CreateBuffer({
            .resource = {
                .heap = RHIDeviceHeapType::Local,
                .host_access = RHIResourceHostAccess::Invisible,
            },
            .usage = RHIBufferUsageBits::VertexBuffer | RHIBufferUsageBits::IndexBuffer | RHIBufferUsageBits::StorageBuffer,
            .size = desc.PrimitiveDataBudget,
        });
        m_instanceData = renderer.GetDevice()->CreateBuffer({
            .resource = {
                .heap = RHIDeviceHeapType::Local,
                .host_access = RHIResourceHostAccess::Invisible,
            },
            .usage = RHIBufferUsageBits::StorageBuffer,
            .size = desc.InstanceDataBudget,
        });
        m_stagingBuffer = m_renderer.GetDevice()->CreateBuffer({
            .resource = {
                .heap = RHIDeviceHeapType::Upload,
                .host_access = RHIResourceHostAccess::WriteOnly,
            },
            .usage = RHIBufferUsageBits::TransferSource,
            .size = m_staging.size(),
        });
        m_globalData = renderer.GetDevice()->CreateBuffer({
            .resource = {
                .heap = RHIDeviceHeapType::Upload,
                .host_access = RHIResourceHostAccess::WriteOnly,
                .coherent = true,
            },
            .usage = RHIBufferUsageBits::UniformBuffer | RHIBufferUsageBits::StorageBuffer,
            .size = desc.GlobalDataBudget,
        });
    }
    RHIBuffer::Arena::Allocation SceneData::PushData(RHIBuffer* buffer, StagingList& staging, StlSpan<const uint8_t> data, size_t alignment)
    {
        auto alloc = buffer->GetArena().Allocate(data.size(), alignment);
        if (alloc == kInvalidHandle)
            throw std::bad_alloc();
        staging.emplace_back(m_staging.size(), buffer->GetArena().GetOffset(alloc), data.size());
        m_staging.insert(m_staging.end(), data.begin(), data.end());
        return alloc;
    }
    void SceneData::UpdateData(RHIBuffer* buffer, StagingList& staging, RHIBuffer::Arena::Allocation handle, StlSpan<const uint8_t> data)
    {
        auto size = buffer->GetArena().GetSize(handle);
        CHECK(data.size() <= size && "New data cannot be larger than inital allocation");
        staging.emplace_back(m_staging.size(), buffer->GetArena().GetOffset(handle), data.size());
        m_staging.insert(m_staging.end(), data.begin(), data.end());
    }
    void FreeData(RHIBuffer* buffer, RHIBuffer::Arena::Allocation alloc) {
        buffer->GetArena().Free(alloc);
    }
    void SceneData::EndTransfer(RHICommandList* cmd) {
        std::memcpy(m_stagingBuffer->Map(), m_staging.data(), m_staging.size());
        m_stagingBuffer->Flush(), m_staging.clear(), m_staging.shrink_to_fit();
        using enum RHIResourceAccessBits;
        using enum RHIPipelineStageBits;        
        cmd->Begin();
        cmd->BeginTransition();
        // Transitions
        if (m_primitiveStaging.size())
            cmd->SetBufferTransition(m_primitiveData.Get(), {
                .src_access = ShaderRead,
                .dst_access = TransferWrite,
                .src_stage = TopOfPipe,
                .dst_stage = Transfer,
            });
        if (m_instanceStaging.size())
            cmd->SetBufferTransition(m_instanceData.Get(), {
                .src_access = ShaderRead,
                .dst_access = TransferWrite,
                .src_stage = TopOfPipe,
                .dst_stage = Transfer,
            });
        cmd->EndTransition();
        // Transfers
        if (m_primitiveStaging.size())
            cmd->CopyBuffer(m_stagingBuffer.Get(), m_primitiveData.Get(), m_primitiveStaging);
        if (m_instanceStaging.size())
            cmd->CopyBuffer(m_stagingBuffer.Get(), m_instanceData.Get(), m_instanceStaging);
        // Transitions
        cmd->BeginTransition();
        if (m_primitiveStaging.size())
            cmd->SetBufferTransition(m_primitiveData.Get(), {
                .src_access = TransferWrite,
                .dst_access = ShaderRead,
                .src_stage = Transfer,
                .dst_stage = BottomOfPipe,
            });
        if (m_instanceStaging.size())
            cmd->SetBufferTransition(m_instanceData.Get(), {
                .src_access = TransferWrite,
                .dst_access = ShaderRead,
                .src_stage = Transfer,
                .dst_stage = BottomOfPipe,
            });
        cmd->EndTransition();
        cmd->End();
        m_primitiveStaging.clear(), m_primitiveStaging.shrink_to_fit();
    }

    bool SceneData::Update(RHICommandList* cmd) {
        if (m_staging.empty())
            return false;
        EndTransfer(cmd);
        return true;
    }

    SceneHandle SceneData::AddPrimitiveData(StlSpan<const uint8_t> data, size_t alignment) {
        auto& [handle, alloc] = m_primitives.allocate();
        alloc = PushData(m_primitiveData.Get(), m_primitiveStaging, data, alignment);
        return handle;
    }
    void SceneData::UpdatePrimitiveData(SceneHandle handle, StlSpan<const uint8_t> data) {
        auto& alloc = m_primitives.at(handle);
        UpdateData(m_primitiveData.Get(), m_primitiveStaging, alloc, data);
    }
    void SceneData::FreePrimitiveData(SceneHandle handle) {
        auto& alloc = m_primitives.at(handle);
        FreeData(m_primitiveData.Get(), alloc);
        m_primitives.free(handle);
    }
    std::pair<size_t, size_t> SceneData::QueryPrimitiveDataSizeAndOffset(SceneHandle handle) const {
        auto& alloc = m_primitives.at(handle);
        return { m_primitiveData->GetArena().GetSize(alloc), m_primitiveData->GetArena().GetOffset(alloc) };
    }

    SceneHandle SceneData::AddInstanceData(StlSpan<const uint8_t> data, size_t alignment) {
        auto& [handle, alloc] = m_instances.allocate();
        alloc = PushData(m_instanceData.Get(), m_instanceStaging, data, alignment);
        return handle;
    }
    void SceneData::UpdateInstanceData(SceneHandle handle, StlSpan<const uint8_t> data) {
        auto& alloc = m_instances.at(handle);
        UpdateData(m_instanceData.Get(), m_instanceStaging, alloc, data);
    }
    void SceneData::FreeInstanceData(SceneHandle handle) {
        auto& alloc = m_instances.at(handle);
        FreeData(m_instanceData.Get(), alloc);
        m_instances.free(handle);
    }
    std::pair<size_t, size_t> SceneData::QueryInstanceDataSizeAndOffset(SceneHandle handle) const {
        auto& alloc = m_instances.at(handle);
        return { m_instanceData->GetArena().GetSize(alloc), m_instanceData->GetArena().GetOffset(alloc) };
    }

    void SceneData::Abort() {
        m_primitiveStaging.clear(), m_primitiveStaging.shrink_to_fit();
        m_instanceStaging.clear(), m_instanceStaging.shrink_to_fit();
    }

    void SceneData::Reset() {
        Abort();
        m_primitives.clear();
        m_instances.clear();
        m_primitiveData->GetArena().Reset();
        m_instanceData->GetArena().Reset();
    }
}
