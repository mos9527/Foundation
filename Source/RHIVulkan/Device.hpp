#pragma once
#include <RHICore/Device.hpp>
#include <SDL3/SDL.h>
#include <vk_mem_alloc.h>

#include "Application.hpp"
#include "Common.hpp"
namespace Foundation::RHI
{
    class VulkanApplication;
    class VulkanDeviceQueue;
    constexpr uint32_t kInvalidQueueIndex = static_cast<uint32_t>(-1);
    struct VulkanDeviceQueues
    {
        RHIObjectPool<VulkanDeviceQueue> storage;
        Handle graphics = kInvalidHandle, compute = kInvalidHandle, transfer = kInvalidHandle;
        VulkanDeviceQueues(Allocator* allocator) : storage(allocator) {};
        VulkanDeviceQueue* Get(Handle handle) const;
        VulkanDeviceQueue* Get(RHIDeviceQueueType type) const
        {
            switch (type)
            {
            case RHIDeviceQueueType::Compute:
                return Get(compute);
            case RHIDeviceQueueType::Transfer:
                return Get(transfer);
            case RHIDeviceQueueType::Present:
            case RHIDeviceQueueType::Graphics:
                return Get(graphics);
            default:
                return nullptr;
            }
        }
        bool IsValid() const
        {
            return graphics != kInvalidHandle && compute != kInvalidHandle && transfer != kInvalidHandle;
        }
    };
    class VulkanDevice;
    class VulkanDeviceSemaphore : public RHIDeviceSemaphore
    {
        const VulkanDevice& mDevice;
        vk::raii::Semaphore mSemaphore{nullptr};

    public:
        VulkanDeviceSemaphore(const VulkanDevice& device, bool is_timeline);
        [[nodiscard]] auto const& GetVkSemaphore() const { return mSemaphore; }

        void DebugSetObjectName(const char* name) override;
    };
    class VulkanDeviceFence : public RHIDeviceFence
    {
        const VulkanDevice& mDevice;
        vk::raii::Fence mFence{nullptr};

    public:
        VulkanDeviceFence(const VulkanDevice& device, bool signaled);
        [[nodiscard]] auto const& GetVkFence() const { return mFence; }

        void DebugSetObjectName(const char* name) override;
    };
    class VulkanDeviceDescriptorSetLayout : public RHIDeviceDescriptorSetLayout
    {
        const VulkanDevice& mDevice;
        vk::raii::DescriptorSetLayout mLayout{nullptr};

    public:
        VulkanDeviceDescriptorSetLayout(const VulkanDevice& device, RHIDeviceDescriptorSetLayoutDesc const& desc);
        [[nodiscard]] auto const& GetVkLayout() const { return mLayout; }

        void DebugSetObjectName(const char* name) override;
    };
    class VulkanDeviceSampler : public RHIDeviceSampler
    {
        const VulkanDevice& mDevice;
        vk::raii::Sampler mSampler{nullptr};

    public:
        VulkanDeviceSampler(const VulkanDevice& device, SamplerDesc const& desc);
        [[nodiscard]] auto const& GetVkSampler() const { return mSampler; }

        void DebugSetObjectName(const char* name) override;
    };
    class VulkanDeviceQueryPool : public RHIDeviceQueryPool
    {
        const VulkanDevice& mDevice;
        const float mTimestampResolution;
        vk::raii::QueryPool mQueryPool{nullptr};

        Vector<uint64_t> mResults;

    public:
        VulkanDeviceQueryPool(const VulkanDevice& device, QueryPoolDesc const& desc);
        [[nodiscard]] auto const& GetVkQueryPool() const { return mQueryPool; }

        const float GetTimestampResolution() override { return mTimestampResolution; }

        void Reset() override;

        Span<const uint64_t> GetResults(bool wait) override;
        void DebugSetObjectName(const char* name) override;
    };
    class VulkanDevice : public RHIDevice
    {
        const VulkanApplication& mApp;

        SDL_Window* mWindow;

        vk::raii::PhysicalDevice mPhysicalDevice{nullptr};
        vk::raii::Device mDevice{nullptr};
        vk::raii::SurfaceKHR mSurface{nullptr};

        Vector<RHISurfaceFormat> mSwapchainFormats;
        Vector<RHISwapchainPresentMode> mSwapchainPresentModes;

        VmaAllocator mVkAllocator{nullptr};
        // Device Object storage
        // Lifetimes and handle dereferencing are managed by the device.
        RHIObjectPool<> mStorage;
        // Queues
        UniquePtr<VulkanDeviceQueues> mQueues{nullptr};

        vk::PhysicalDeviceProperties mPhysicalDeviceProperties{};
        RHIPipelineStateCacheKey mPipelineCacheKey{};
        RHIDeviceCapabilities mDeviceCaps{};
    public:
        VulkanDevice(VulkanApplication const& app, vk::raii::PhysicalDevice physicalDevice,
                     SDL_Window* window = nullptr);
        ~VulkanDevice() override;

        RHIDeviceCapabilities GetCapabilities() const override { return mDeviceCaps; }
        RHIPipelineStateCacheKey GetPipelineCacheKey() const override { return mPipelineCacheKey; }

        RHIDeviceQueue* GetDeviceQueue(RHIDeviceQueueType type) const override;

        Span<RHISurfaceFormat const> GetSwapchainSupportedFormats() const override;
        Span<RHISwapchainPresentMode const> GetSwapchainSupportedPresentModes() const override;
        RHIDeviceScopedHandle<RHISwapchain> CreateSwapchain(RHISwapchain::SwapchainDesc const& desc) override;
        RHISwapchain* GetSwapchain(Handle handle) const override;
        void DestroySwapchain(Handle handle) override;

        RHIDeviceScopedHandle<RHIPipelineStateCache>
        CreatePipelineCache(RHIPipelineStateCache::PipelineStateCacheDesc const& desc) override;
        RHIPipelineStateCache* GetPipelineCache(Handle handle) const override;
        void DestroyPipelineCache(Handle handle) override;

        RHIDeviceScopedHandle<RHIPipelineState>
        CreatePipelineState(RHIPipelineState::PipelineStateDesc const& desc) override;
        RHIPipelineState* GetPipelineState(Handle handle) const override;
        void DestroyPipelineState(Handle handle) override;

        RHIDeviceScopedHandle<RHIShaderModule>
        CreateShaderModule(RHIShaderModule::ShaderModuleDesc const& desc) override;
        RHIShaderModule* GetShaderModule(Handle handle) const override;
        void DestroyShaderModule(Handle handle) override;

        RHIDeviceScopedHandle<RHICommandPool> CreateCommandPool(RHICommandPool::PoolDesc desc) override;
        RHICommandPool* GetCommandPool(Handle handle) const override;
        void DestroyCommandPool(Handle handle) override;

        RHIDeviceScopedHandle<RHIDeviceSemaphore> CreateSemaphore(bool is_timeline) override;
        RHIDeviceSemaphore* GetSemaphore(Handle handle) const override;
        void DestroySemaphore(Handle handle) override;

        RHIDeviceScopedHandle<RHIDeviceFence> CreateFence(bool signaled) override;
        RHIDeviceFence* GetFence(Handle handle) const override;
        void DestroyFence(Handle handle) override;

        RHIDeviceScopedHandle<RHIBuffer> CreateBuffer(RHIBufferDesc const& desc) override;
        RHIBuffer* GetBuffer(Handle handle) const override;
        void DestroyBuffer(Handle handle) override;

        RHIDeviceScopedHandle<RHITexture> CreateTexture(RHITextureDesc const& desc) override;
        RHITexture* GetTexture(Handle handle) const override;
        void DestroyTexture(Handle handle) override;

        [[nodiscard]] RHIDeviceScopedHandle<RHIAccelerationStructure>
        CreateAccelerationStructure(RHIAccelerationStructureDesc const& desc) override;
        [[nodiscard]] RHIAccelerationStructure* GetAccelerationStructure(Handle handle) const override;
        void DestroyAccelerationStructure(Handle handle) override;

        RHIDeviceScopedHandle<RHIDeviceDescriptorSetLayout>
        CreateDescriptorSetLayout(RHIDeviceDescriptorSetLayoutDesc const& desc) override;
        RHIDeviceDescriptorSetLayout* GetDescriptorSetLayout(Handle handle) const override;
        void DestroyDescriptorSetLayout(Handle handle) override;

        RHIDeviceScopedHandle<RHIDeviceDescriptorPool>
        CreateDescriptorPool(RHIDeviceDescriptorPool::PoolDesc const& desc) override;
        RHIDeviceDescriptorPool* GetDescriptorPool(Handle handle) const override;
        void DestroyDescriptorPool(Handle handle) override;

        RHIDeviceScopedHandle<RHIDeviceSampler> CreateSampler(RHIDeviceSampler::SamplerDesc const& desc) override;
        RHIDeviceSampler* GetSampler(Handle handle) const override;
        void DestroySampler(Handle handle) override;

        RHIDeviceScopedHandle<RHIDeviceQueryPool>
        CreateQueryPool(RHIDeviceQueryPool::QueryPoolDesc const& desc) override;
        RHIDeviceQueryPool* GetQueryPool(Handle handle) const override;
        void DestroyQueryPool(Handle handle) override;

        [[nodiscard]] RHIAccelerationStructureSizeInfo GetAccelerationStructureSizeInfo(
                    RHIAccelerationStructureBuildDesc const& desc,
                    Allocator* scratchAllocator = nullptr
        ) const override;
        [[nodiscard]] size_t
        WriteAccelerationStructureInstanceData(RHIAccelerationStructureGeometryInstance const& data,
                                               void* dest) const override;

        void ResetFences(Span<RHIDeviceFence* const> fences) override;
        bool WaitForFences(Span<RHIDeviceFence* const> fences, bool wait_all, size_t timeout) override;

        void SignalTimelineSemaphores(Span<const Pair<RHIDeviceSemaphore*, size_t>> semaphores) override;
        bool WaitForTimelineSemaphores(Span<const Pair<RHIDeviceSemaphore*, size_t>> semaphores,
                                       size_t timeout) override;

        void WaitIdle() const override;

        void QueryBudget(size_t& used, size_t& budget) const override;
        void QueryAllocationStats(size_t& blockBytes, size_t& allocationBytes) const override;
        void QueryMemoryStats(RHIDeviceMemoryStats& outStats) const override;
        String QueryDeviceString() const override;

        Allocator* GetAllocator() const;

        auto const& GetVkQueues() const { return mQueues; }
        auto const& GetVkDevice() const { return mDevice; }
        auto const& GetVkSurface() const { return mSurface; }
        auto const& GetVkPhysicalDevice() const { return mPhysicalDevice; }
        auto const& GetVkPhysicalDeviceProperties() const { return mPhysicalDeviceProperties; }
        auto const& GetVkAllocator() const { return mVkAllocator; }
        [[nodiscard]] vk::AllocationCallbacks const* GetVkAllocationCallbacks() const;
        [[nodiscard]] VkAllocationCallbacks const* GetVkAllocationCallbacksNative() const;

        void DebugSetObjectName(const char* name) override;
    };
    class VulkanDeviceQueue : public RHIDeviceQueue
    {
        const VulkanDevice& mDevice;
        const uint32_t mQueueIndex;
        const uint32_t mFamilyIndex;
        vk::raii::Queue mQueue{nullptr};

    public:
        VulkanDeviceQueue(const VulkanDevice& device, uint32_t family_index, uint32_t queue_index) :
            RHIDeviceQueue(device), mDevice(device), mQueueIndex(queue_index), mFamilyIndex(family_index),
            mQueue(device.GetVkDevice(), family_index, queue_index)
        {
        }

        [[nodiscard]] const VulkanDevice& GetVulkanDevice() const { return mDevice; }
        [[nodiscard]] vk::raii::Queue GetVkQueue() const { return mQueue; }
        [[nodiscard]] uint32_t GetVkQueueIndex() const { return mQueueIndex; }
        [[nodiscard]] uint32_t GetVkQueueFamily() const override { return mFamilyIndex; }

        void WaitIdle() const override;
        void Submit(Span<const SubmitDesc>, RHIDeviceFence* completionFence) const override;
        void Present(PresentDesc const& desc) const override;


        void DebugSetObjectName(const char* name) override;
    };
} // namespace Foundation::RHI