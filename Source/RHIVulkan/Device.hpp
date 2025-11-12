#pragma once
#include <SDL3/SDL.h>
#include <vk_mem_alloc.h>
#include <RHICore/Device.hpp>
#include "Common.hpp"
namespace Foundation::RHI {
    class VulkanApplication;
    class VulkanDeviceQueue;
    constexpr uint32_t kInvalidQueueIndex = static_cast<uint32_t>(-1);
    struct VulkanDeviceQueues
    {
        RHIObjectPool<VulkanDeviceQueue> storage;
        Handle graphics = kInvalidHandle, compute = kInvalidHandle, transfer = kInvalidHandle;
        VulkanDeviceQueues(Allocator* allocator) : storage(allocator) {};
        VulkanDeviceQueue* Get(Handle handle) const;
        VulkanDeviceQueue* Get(RHIDeviceQueueType type) const {
            switch (type) {
            case RHIDeviceQueueType::Compute:  return Get(compute);
            case RHIDeviceQueueType::Transfer: return Get(transfer);
            case RHIDeviceQueueType::Present:
            case RHIDeviceQueueType::Graphics: return Get(graphics);
            default:
                CHECK_MSG(false, "Unknown queue type {}", type);
            }
        }
        bool IsValid() const {
            return graphics != kInvalidHandle &&
                compute != kInvalidHandle &&
                transfer != kInvalidHandle;
        }
        bool IsDedicatedCompute() const {
            return IsValid() && compute != graphics;
        }
        bool IsDedicatedTransfer() const {
            return IsValid() && transfer != graphics;
        }
    };
    class VulkanDevice;
    class VulkanDeviceSemaphore : public RHIDeviceSemaphore {
        const VulkanDevice& mDevice;
        vk::raii::Semaphore mSemaphore{ nullptr };
    public:
        VulkanDeviceSemaphore(const VulkanDevice& device, bool is_timeline);
        [[nodiscard]] auto const& GetVkSemaphore() const { return mSemaphore; }

        void DebugSetObjectName(const char* name) override;
    };
    class VulkanDeviceFence : public RHIDeviceFence {
        const VulkanDevice& mDevice;
        vk::raii::Fence mFence{ nullptr };
    public:
        VulkanDeviceFence(const VulkanDevice& device, bool signaled);
        [[nodiscard]] auto const& GetVkFence() const { return mFence; }

        void DebugSetObjectName(const char* name) override;
    };
    class VulkanDeviceDescriptorSetLayout : public RHIDeviceDescriptorSetLayout {
        const VulkanDevice& mDevice;
        vk::raii::DescriptorSetLayout mLayout{ nullptr };
    public:
        VulkanDeviceDescriptorSetLayout(const VulkanDevice& device, RHIDeviceDescriptorSetLayoutDesc const& desc);
        [[nodiscard]] auto const& GetVkLayout() const { return mLayout; }

        void DebugSetObjectName(const char* name) override;
    };
    class VulkanDeviceSampler : public RHIDeviceSampler {
        const VulkanDevice& mDevice;
        vk::raii::Sampler mSampler{ nullptr };
    public:
        VulkanDeviceSampler(const VulkanDevice& device, SamplerDesc const& desc);
        [[nodiscard]] auto const& GetVkSampler() const { return mSampler; }

        void DebugSetObjectName(const char* name) override;
    };
    class VulkanDevice : public RHIDevice {
        const VulkanApplication& mApp;

        SDL_Window* mWindow;

        vk::PhysicalDeviceProperties mProperties;
        vk::raii::PhysicalDevice mPhysicalDevice{ nullptr };
        vk::raii::Device mDevice{ nullptr };
        vk::raii::SurfaceKHR mSurface{ nullptr };

        Vector<RHIResourceFormat> mSwapchainFormats;
        Vector<RHISwapchainPresentMode> mSwapchainPresentModes;

        VmaAllocator mVkAllocator{ nullptr };
        // Device Object storage
        // Lifetimes and handle dereferencing are managed by the device.
        RHIObjectPool<> mStorage;
        // Queues
        UniquePtr<VulkanDeviceQueues> mQueues{ nullptr };
    public:
        VulkanDevice(VulkanApplication const& app, vk::raii::PhysicalDevice physicalDevice, SDL_Window* window = nullptr);
        ~VulkanDevice() override;

        RHIDeviceQueue* GetDeviceQueue(RHIDeviceQueueType type) const override;

        Span<RHIResourceFormat const> GetSwapchainSupportedFormats() const override;
        Span<RHISwapchainPresentMode const> GetSwapchainSupportedPresentModes() const override;
        RHIDeviceScopedObjectHandle<RHISwapchain> CreateSwapchain(RHISwapchain::SwapchainDesc const& desc) override;
        RHISwapchain* GetSwapchain(Handle handle) const override;
        void DestroySwapchain(Handle handle) override;

        RHIDeviceScopedObjectHandle<RHIPipelineState> CreatePipelineState(RHIPipelineState::PipelineStateDesc const& desc) override;
        RHIPipelineState* GetPipelineState(Handle handle) const override;
        void DestroyPipelineState(Handle handle) override;

        RHIDeviceScopedObjectHandle<RHIShaderModule> CreateShaderModule(RHIShaderModule::ShaderModuleDesc const& desc) override;
        RHIShaderModule* GetShaderModule(Handle handle) const override;
        void DestroyShaderModule(Handle handle) override;

        RHIDeviceScopedObjectHandle<RHICommandPool> CreateCommandPool(RHICommandPool::PoolDesc desc) override;
        RHICommandPool* GetCommandPool(Handle handle) const override;
        void DestroyCommandPool(Handle handle) override;

        RHIDeviceScopedObjectHandle<RHIDeviceSemaphore> CreateSemaphore(bool is_timeline) override;
        RHIDeviceSemaphore* GetSemaphore(Handle handle) const override;
        void DestroySemaphore(Handle handle) override;

        RHIDeviceScopedObjectHandle<RHIDeviceFence> CreateFence(bool signaled) override;
        RHIDeviceFence* GetFence(Handle handle) const override;
        void DestroyFence(Handle handle) override;

        RHIDeviceScopedObjectHandle<RHIBuffer> CreateBuffer(RHIBufferDesc const& desc) override;
        RHIBuffer* GetBuffer(Handle handle) const override;
        void DestroyBuffer(Handle handle) override;

        RHIDeviceScopedObjectHandle<RHITexture> CreateTexture(RHITextureDesc const& desc) override;
        RHITexture* GetImage(Handle handle) const override;
        void DestroyImage(Handle handle) override;

        RHIDeviceScopedObjectHandle<RHIDeviceDescriptorSetLayout> CreateDescriptorSetLayout(RHIDeviceDescriptorSetLayoutDesc const& desc) override;
        RHIDeviceDescriptorSetLayout* GetDescriptorSetLayout(Handle handle) const override;
        void DestroyDescriptorSetLayout(Handle handle) override;

        RHIDeviceScopedObjectHandle<RHIDeviceDescriptorPool> CreateDescriptorPool(
            RHIDeviceDescriptorPool::PoolDesc const& desc) override;
        RHIDeviceDescriptorPool* GetDescriptorPool(Handle handle) const override;
        void DestroyDescriptorPool(Handle handle) override;

        RHIDeviceScopedObjectHandle<RHIDeviceSampler> CreateSampler(RHIDeviceSampler::SamplerDesc const& desc) override;
        RHIDeviceSampler* GetSampler(Handle handle) const override;
        void DestroySampler(Handle handle) override;

        void ResetFences(Span<RHIDeviceFence*> fences) override;
        void WaitForFences(Span<RHIDeviceFence*> fences, bool wait_all, size_t timeout) override;

        void SignalTimelineSemaphores(Span<const Pair<RHIDeviceSemaphore*, size_t>> semaphores) override;
        bool WaitForTimelineSemaphores(Span<const Pair<RHIDeviceSemaphore*, size_t>> semaphores, size_t timeout) override;

        void WaitIdle() const override;

        Allocator* GetAllocator() const;
        vk::AllocationCallbacks const& GetVkAllocatorCallbacks() const;

        auto const& GetVkQueues() const { return mQueues; }
        auto const& GetVkDevice() const { return mDevice; }
        auto const& GetVkSurface() const { return mSurface; }
        auto const& GetVkPhysicalDevice() const { return mPhysicalDevice; }
        auto const& GetVkAllocator() const { return mVkAllocator; }

        void DebugSetObjectName(const char* name) override;
    };
    class VulkanDeviceQueue : public RHIDeviceQueue {
        const VulkanDevice& mDevice;
        const uint32_t mQueueIndex;
        const uint32_t mFamilyIndex;
        vk::raii::Queue mQueue{ nullptr };
    public:
        VulkanDeviceQueue(const VulkanDevice& device, uint32_t family_index, uint32_t queue_index)
            : RHIDeviceQueue(device), mDevice(device), mQueueIndex(queue_index), mFamilyIndex(family_index),
        mQueue(device.GetVkDevice(), family_index, queue_index) {
        };

        [[nodiscard]] const VulkanDevice& GetVulkanDevice() const { return mDevice; }
        [[nodiscard]] vk::raii::Queue GetVkQueue() const { return mQueue; }
        [[nodiscard]] uint32_t GetVkQueueIndex() const { return mQueueIndex; }

        void WaitIdle() const override;
        void Submit(Span<const SubmitDesc>, RHIDeviceFence* completionFence) const override;
        void Present(PresentDesc const& desc) const override;
        [[nodiscard]] uint32_t GetQueueIndex() const override { return GetVkQueueIndex(); }

        void DebugSetObjectName(const char* name) override;
    };
}
