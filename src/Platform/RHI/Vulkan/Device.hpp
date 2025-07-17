#pragma once
#include <Platform/RHI/Device.hpp>
#include <Platform/RHI/Vulkan/Common.hpp>
#include <vma/vk_mem_alloc.h>
namespace Foundation {
    namespace Platform {
        namespace RHI {
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
                VulkanDeviceSemaphore(const VulkanDevice& device);
                inline auto const& GetVkSemaphore() const { return m_semaphore; }

                inline bool IsValid() const override { return m_semaphore != nullptr; }
                inline const char* GetName() const override { return "VulkanDeviceSemaphore"; }
            };
            class VulkanDeviceFence : public RHIDeviceFence {
                const VulkanDevice& m_device;
                vk::raii::Fence m_fence{ nullptr };
            public:
                VulkanDeviceFence(const VulkanDevice& device, bool signaled);                
                inline auto const& GetVkFence() const { return m_fence; }

                inline bool IsValid() const override { return m_fence != nullptr; }
                inline const char* GetName() const override { return "VulkanDeviceFence"; }
            };
            class VulkanDevice : public RHIDevice {
                const VulkanApplication& m_app;

                vk::PhysicalDeviceProperties m_properties;
                vk::raii::PhysicalDevice m_physicalDevice{ nullptr };
                vk::raii::Device m_device{ nullptr };
                vk::raii::SurfaceKHR m_surface{ nullptr };

                VmaAllocator m_vkAllocator{ nullptr };
                // Queues
                Core::UniquePtr<VulkanDeviceQueues> m_queues{ nullptr };
                // Device Object storage
                // Lifetimes and handle dereferencing are managed by the device.
                RHIObjectStorage<> m_storage;
                void Instantiate(Window* win = nullptr);
            public:
                VulkanDevice(Window* window, VulkanApplication const& app, const vk::raii::PhysicalDevice& physicalDevice);
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

                RHIDeviceScopedObjectHandle<RHIDeviceSemaphore> CreateSemaphore() override;
                RHIDeviceSemaphore* GetSemaphore(Handle handle) const override;
                void DestroySemaphore(Handle handle) override;

                RHIDeviceScopedObjectHandle<RHIDeviceFence> CreateFence(bool signaled = false) override;
                RHIDeviceFence* GetFence(Handle handle) const override;
                void DestroyFence(Handle handle) override;

                RHIDeviceScopedObjectHandle<RHIBuffer> CreateBuffer(RHIBufferDesc const& desc) override;
                RHIBuffer* GetBuffer(Handle handle) const override;
                void DestroyBuffer(Handle handle) override;

                RHIDeviceScopedObjectHandle<RHIImage> CreateImage(RHIImageDesc const& desc) override;
                RHIImage* GetImage(Handle handle) const override;
                void DestroyImage(Handle handle) override;

                void ResetFences(Core::StlSpan<const RHIDeviceObjectHandle<RHIDeviceFence>> fences) override;
                void WaitForFences(Core::StlSpan<const RHIDeviceObjectHandle<RHIDeviceFence>> fences, bool wait_all, size_t timeout) override;

                inline const char* GetName() const override { return m_properties.deviceName.data(); }
                inline bool IsValid() const override { return m_device != nullptr; }

                Core::Allocator* GetAllocator() const;
                vk::AllocationCallbacks const& GetVkAllocatorCallbacks() const;

                inline auto const& GetVkQueues() const { return m_queues; }
                inline auto const& GetVkDevice() const { return m_device; }
                inline auto const& GetVkSurface() const { return m_surface; }
                inline auto const& GetVkPhysicalDevice() const { return m_physicalDevice; }
                inline auto const& GetVkAllocator() const { return m_vkAllocator; }
            };
            class VulkanDeviceQueue : public RHIDeviceQueue {
                const VulkanDevice& m_device;
                const uint32_t m_queue_index;
                vk::raii::Queue m_queue{ nullptr };

                const std::string m_name;
            public:
                VulkanDeviceQueue(const VulkanDevice& device, uint32_t queue_index, std::string const& name)
                    : RHIDeviceQueue(device), m_device(device), m_queue(device.GetVkDevice(), queue_index, 0), m_queue_index(queue_index), m_name(name) {};

                inline const VulkanDevice& GetVulkanDevice() const { return m_device; }
                inline vk::raii::Queue GetVkQueue() const { return m_queue; }
                inline uint32_t GetVkQueueIndex() const { return m_queue_index; }

                void WaitIdle() const override;
                void Submit(SubmitDesc const& desc) const override;
                void Present(PresentDesc const& desc) const override;

                inline const char* GetName() const override {
                    return m_name.c_str();
                }
                inline bool IsValid() const override {
                    return m_queue != nullptr;
                }
            };
        }
    }
}
