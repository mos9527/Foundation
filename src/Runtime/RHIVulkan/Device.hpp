#pragma once
#include <Native/Application.hpp>
#include <RHICore/Device.hpp>
#include <vma/vk_mem_alloc.h>
#include "Common.hpp"
namespace Foundation::RHI {
    class VulkanApplication;
    class VulkanDeviceQueue;
    constexpr size_t kDeviceStorageReserveSize = 65536;
    constexpr uint32_t kInvalidQueueIndex = static_cast<uint32_t>(-1);
    struct VulkanDeviceQueues
    {
        RHIObjectStorage<VulkanDeviceQueue> storage;
        Handle graphics = kInvalidHandle, compute = kInvalidHandle, transfer = kInvalidHandle, present = kInvalidHandle;
        VulkanDeviceQueues(Core::Allocator* allocator) : storage(allocator) {};
        VulkanDeviceQueue* Get(Handle handle) const;
        inline VulkanDeviceQueue* Get(RHIDeviceQueueType type) const {
            switch (type) {
            case RHIDeviceQueueType::Present:  return Get(present);
            case RHIDeviceQueueType::Compute:  return Get(compute);
            case RHIDeviceQueueType::Transfer: return Get(transfer);
            default:
            case RHIDeviceQueueType::Graphics: return Get(graphics);
            }
        }
        inline bool IsValid() const {
            return graphics != kInvalidHandle &&
                compute != kInvalidHandle &&
                transfer != kInvalidHandle;
        }
        inline bool CanPresent() const {
            return present != kInvalidHandle;
        }
        inline bool IsDedicatedCompute() const {
            return IsValid() && compute != graphics;
        }
        inline bool IsDedicatedTransfer() const {
            return IsValid() && transfer != graphics;
        }
        inline bool IsDedicatedPresent() const {
            return IsValid() && present != graphics;
        }
    };
    class VulkanDevice;
    class VulkanDeviceSemaphore : public RHIDeviceSemaphore {
        const VulkanDevice& m_device;
        vk::raii::Semaphore m_semaphore{ nullptr };
    public:
        VulkanDeviceSemaphore(const VulkanDevice& device, bool is_timeline);
        inline auto const& GetVkSemaphore() const { return m_semaphore; }

        void DebugSetObjectName(const char* name) override;
    };
    class VulkanDeviceFence : public RHIDeviceFence {
        const VulkanDevice& m_device;
        vk::raii::Fence m_fence{ nullptr };
    public:
        VulkanDeviceFence(const VulkanDevice& device, bool signaled);
        inline auto const& GetVkFence() const { return m_fence; }

        void DebugSetObjectName(const char* name) override;
    };
    class VulkanDeviceDescriptorSetLayout : public RHIDeviceDescriptorSetLayout {
        const VulkanDevice& m_device;
        vk::raii::DescriptorSetLayout m_layout{ nullptr };
    public:
        VulkanDeviceDescriptorSetLayout(const VulkanDevice& device, RHIDeviceDescriptorSetLayoutDesc const& desc);
        inline auto const& GetVkLayout() const { return m_layout; }

        void DebugSetObjectName(const char* name) override;
    };
    class VulkanDeviceSampler : public RHIDeviceSampler {
        const VulkanDevice& m_device;
        vk::raii::Sampler m_sampler{ nullptr };
    public:
        VulkanDeviceSampler(const VulkanDevice& device, RHIDeviceSampler::SamplerDesc const& desc);
        inline auto const& GetVkSampler() const { return m_sampler; }

        void DebugSetObjectName(const char* name) override;
    };
    class VulkanDevice : public RHIDevice {
        const VulkanApplication& m_app;

        vk::PhysicalDeviceProperties m_properties;
        vk::raii::PhysicalDevice m_physicalDevice{ nullptr };
        vk::raii::Device m_device{ nullptr };
        vk::raii::SurfaceKHR m_surface{ nullptr };

        VmaAllocator m_vkAllocator{ nullptr };
        // Device Object storage
        // Lifetimes and handle dereferencing are managed by the device.
        RHIObjectStorage<> m_storage;
        // Queues
        Core::UniquePtr<VulkanDeviceQueues> m_queues{ nullptr };
    public:
        VulkanDevice(VulkanApplication const& app, const vk::raii::PhysicalDevice& physicalDevice, Native::Window* window);
        ~VulkanDevice();

        void DebugLogDeviceInfo() const;
        void DebugLogAllocatorInfo() const;

        RHIDeviceQueue* GetDeviceQueue(RHIDeviceQueueType type) const override;

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

        RHIDeviceScopedObjectHandle<RHIDeviceSemaphore> CreateSemaphore(bool is_timeline = false) override;
        RHIDeviceSemaphore* GetSemaphore(Handle handle) const override;
        void DestroySemaphore(Handle handle) override;

        RHIDeviceScopedObjectHandle<RHIDeviceFence> CreateFence(bool signaled = false) override;
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

        void ResetFences(Core::StlSpan<const RHIDeviceObjectHandle<RHIDeviceFence>> fences) override;
        void WaitForFences(Core::StlSpan<const RHIDeviceObjectHandle<RHIDeviceFence>> fences, bool wait_all, size_t timeout) override;

        void SignalTimelineSemaphores(Core::StlSpan<const std::pair<RHIDeviceObjectHandle<RHIDeviceSemaphore>, size_t>> semaphores) override;
        void WaitForTimelineSemaphores(Core::StlSpan<const std::pair<RHIDeviceObjectHandle<RHIDeviceSemaphore>, size_t>> semaphores, size_t timeout) override;

        void WaitIdle() const override;

        Core::Allocator* GetAllocator() const;
        vk::AllocationCallbacks const& GetVkAllocatorCallbacks() const;

        inline auto const& GetVkQueues() const { return m_queues; }
        inline auto const& GetVkDevice() const { return m_device; }
        inline auto const& GetVkSurface() const { return m_surface; }
        inline auto const& GetVkPhysicalDevice() const { return m_physicalDevice; }
        inline auto const& GetVkAllocator() const { return m_vkAllocator; }

        void DebugSetObjectName(const char* name) override;
    };
    class VulkanDeviceQueue : public RHIDeviceQueue {
        const VulkanDevice& m_device;
        const uint32_t m_queue_index;
        vk::raii::Queue m_queue{ nullptr };
    public:
        VulkanDeviceQueue(const VulkanDevice& device, uint32_t queue_index)
            : RHIDeviceQueue(device), m_device(device), m_queue(device.GetVkDevice(), queue_index, 0), m_queue_index(queue_index) {
        };

        inline const VulkanDevice& GetVulkanDevice() const { return m_device; }
        inline vk::raii::Queue GetVkQueue() const { return m_queue; }
        inline uint32_t GetVkQueueIndex() const { return m_queue_index; }

        void WaitIdle() const override;
        void Submit(SubmitDesc const& desc) const override;
        void Present(PresentDesc const& desc) const override;

        void DebugSetObjectName(const char* name) override;
    };
}
