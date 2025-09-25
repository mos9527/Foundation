#include "VirtualAllocator.hpp"
#include <RHIVulkan/Application.hpp>
namespace Foundation::Rendering
{
    VirtualAllocator::VirtualAllocator(size_t size, Allocator* alloc) : mSize(size), mAllocs(alloc)
    {
        VkAllocationCallbacks callbacks = RHI::CreateVulkanCpuAllocationCallbacks(alloc);
        VmaVirtualBlockCreateInfo info{.size = size, .pAllocationCallbacks = &callbacks};
        vmaCreateVirtualBlock(&info, &mBlock);
    }
    VirtualAllocation VirtualAllocator::Allocate(size_t size, size_t alignment) {
        std::scoped_lock lock(mMutex);
        VmaVirtualAllocationCreateInfo info{ .size = size, .alignment = alignment };
        VmaVirtualAllocation alloc{};
        VkDeviceSize offset{};
        VkResult ret = vmaVirtualAllocate(mBlock, &info, &alloc, &offset);
        if (ret != VK_SUCCESS)
            return kInvalidVirtualAllocation;
        auto [handle, ainfo] = mAllocs.pop_pair();
        auto& [sz, off, vmaAlloc] = ainfo;
        sz = size, off = offset, vmaAlloc = alloc;
        return handle;
    }
    void VirtualAllocator::Free(VirtualAllocation handle)
    {
        std::scoped_lock lock(mMutex);
        auto& [sz, off, vmaAlloc] = mAllocs.at(handle);
        vmaVirtualFree(mBlock, vmaAlloc);
        mAllocs.free(handle);
    }
    Pair<size_t, size_t> VirtualAllocator::Query(VirtualAllocation handle)
    {
        auto const& [sz, off, vmaAlloc] = mAllocs.at(handle);
        return { off, sz };
    }
    VirtualAllocator::~VirtualAllocator()
    {
        vmaClearVirtualBlock(mBlock);
        vmaDestroyVirtualBlock(mBlock);
    }
}