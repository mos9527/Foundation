#include "VirtualAllocator.hpp"
#include <RHIVulkan/Application.hpp>
namespace Foundation::Rendering
{
    VirtualAllocator::VirtualAllocator(size_t size, Allocator* alloc) : m_size(size), m_allocs(alloc)
    {
        VkAllocationCallbacks callbacks = RHI::CreateVulkanCpuAllocationCallbacks(alloc);
        VmaVirtualBlockCreateInfo info{.size = size, .pAllocationCallbacks = &callbacks};
        vmaCreateVirtualBlock(&info, &m_block);
    }
    VirtualAllocation VirtualAllocator::Allocate(size_t size, size_t alignment) {
        std::scoped_lock lock(m_mutex);
        VmaVirtualAllocationCreateInfo info{ .size = size, .alignment = alignment };
        VmaVirtualAllocation alloc{};
        VkDeviceSize offset{};
        VkResult ret = vmaVirtualAllocate(m_block, &info, &alloc, &offset);
        if (ret != VK_SUCCESS)
            return kInvalidVirtualAllocation;
        auto& [handle, ainfo] = m_allocs.pop_pair();
        auto& [sz, off, vmaAlloc] = ainfo;
        sz = size, off = offset, vmaAlloc = alloc;
        return handle;
    }
    void VirtualAllocator::Free(VirtualAllocation handle)
    {
        std::scoped_lock lock(m_mutex);
        auto& [sz, off, vmaAlloc] = m_allocs.at(handle);
        vmaVirtualFree(m_block, vmaAlloc);
        m_allocs.free(handle);
    }
    Pair<size_t, size_t> VirtualAllocator::Query(VirtualAllocation handle)
    {
        auto const& [sz, off, vmaAlloc] = m_allocs.at(handle);
        return { off, sz };
    }
    VirtualAllocator::~VirtualAllocator()
    {
        vmaClearVirtualBlock(m_block);
        vmaDestroyVirtualBlock(m_block);
    }
}