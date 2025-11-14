#pragma once
#include "PipelineState.hpp"
#include "Shader.hpp"
#include "Swapchain.hpp"
#include "Command.hpp"
#include "Resource.hpp"
#include "Descriptor.hpp"

namespace Foundation::RHI {
    class RHIApplication;
    class RHIDevice;
    class RHIDeviceSemaphore;
    class RHIDeviceFence;
    class RHIDeviceQueue : public RHIObject {
    protected:
        const RHIDevice& mDevice;
    public:
        RHIDeviceQueue(RHIDevice const& device) : mDevice(device) {}

        virtual void WaitIdle() const = 0;
        using TimelinePair = Pair<RHIDeviceSemaphore*, size_t>;
        struct SubmitDesc {
            // Semaphore(s) and the minimum values to wait on
            Span<const TimelinePair> timelineWaits;
            // Semaphore(s) and the values to signal
            Span<const TimelinePair> timelineSignals;
            // Binary semaphore(s) to wait when the command lists are done
            Span<RHIDeviceSemaphore* const> waits;
            // Stages the semaphore waits occur in [timeline_waits..., waits...]
            Span<const RHIPipelineStage> waitsStages{};
            // Binary semaphore(s) to signal when the command lists are done
            Span<RHIDeviceSemaphore* const> signals;
            // Command lists to submit
            Span<RHICommandList* const> cmdLists;
        };
        virtual void Submit(Span<const SubmitDesc> desc, RHIDeviceFence* completionFence = nullptr) const = 0;

        struct PresentDesc {
            uint32_t imageIndex;
            RHISwapchain* swapchain;
            // Binary semaphore(s) to wait
            Span<RHIDeviceSemaphore* const> waits;
        };
        virtual void Present(PresentDesc const& desc) const = 0;
        [[nodiscard]] virtual uint32_t GetVkQueueFamily() const = 0;
        virtual void DebugSetObjectName(const char* name) = 0;
    };
    // https://docs.vulkan.org/samples/latest/samples/extensions/timeline_semaphore/README.html
    class RHIDeviceSemaphore : public RHIObject {
    protected:
        const RHIDevice& mDevice;
    public:
        const bool mIsTimeline;
        RHIDeviceSemaphore(RHIDevice const& device, bool is_timeline) : mDevice(device), mIsTimeline(is_timeline) {}

        virtual void DebugSetObjectName(const char* name) = 0;
    };
    class RHIDeviceFence : public RHIObject {
    protected:
        const RHIDevice& mDevice;
    public:
        RHIDeviceFence(RHIDevice const& device) : mDevice(device) {}

        virtual void DebugSetObjectName(const char* name) = 0;
    };
    struct RHIDeviceDescriptorSetLayoutDesc {
        struct Binding {
            uint32_t count{ 1 }; // Array size for array access
            RHIShaderStage stage{ RHIShaderStageBits::All }; // Stage this binding is used in
            RHIDescriptorType type; // Type of this binding
        };
        // Bindings that make up this layout
        Span<const Binding> bindings;
        // Allow updating descriptors after being bound to a command buffer when
        // they are not used
        bool updateAfterBind{ false };
    };
    class RHIDeviceDescriptorSetLayout : public RHIObject {
    protected:
        const RHIDevice& mDevice;
    public:
        const RHIDeviceDescriptorSetLayoutDesc mDesc;
        RHIDeviceDescriptorSetLayout(RHIDevice const& device, RHIDeviceDescriptorSetLayoutDesc const& desc)
            : mDevice(device), mDesc(desc) {
        }

        virtual void DebugSetObjectName(const char* name) = 0;
    };
    class RHIDeviceSampler : public RHIObject {
    protected:
        const RHIDevice& mDevice;
    public:
        struct SamplerDesc {
            struct Anisotropy {
                bool enable{ false }; // Enable anisotropic filtering
                float maxLevel{ 16.0f }; // Max anisotropy level
            } anisotropy;
            struct AddressMode {
                enum Mode {
                    Repeat,
                    MirroredRepeat,
                    ClampToEdge,
                    ClampToBorder,
                    MirrorClampToEdge
                } u{ Repeat }, v{ Repeat }, w{ Repeat }; // Address modes for U, V, W coordinates
            } addressMode;
            struct Mipmap {
                enum MipmapMode {
                    Linear,
                    Nearest
                } mipmapMode{ Linear }; // Mipmap mode;
                float bias{ 0.0f }; // Mipmap LOD bias
            } mipmap;
            struct Filter {
                enum Type {
                    NearestNeighbor,
                    Linear,
                    Cubic
                } minFilter{ Linear }, magFilter{ Linear }; // Minification and magnification filters
            } filter;
            struct LOD {
                float min{ 0.0f }; // Minimum level of detail
                float max{ 16.0f }; // Maximum level of detail                        
            } lod; // Level of detail settings;
        } const mDesc;
        RHIDeviceSampler(RHIDevice const& device, SamplerDesc const& desc)
            : mDevice(device), mDesc(desc) {
        }

        virtual void DebugSetObjectName(const char* name) = 0;
    };
    class RHIDevice : public RHIObject {
    protected:
        const RHIApplication& mApp;
    public:
        struct DeviceDesc {
            uint32_t id = 0;
            const char* name = nullptr;
        };
        RHIDevice(RHIApplication const& app) : mApp(app) {}

        [[nodiscard]] virtual Span<RHIResourceFormat const> GetSwapchainSupportedFormats() const = 0;
        [[nodiscard]] virtual Span<RHISwapchainPresentMode const> GetSwapchainSupportedPresentModes() const = 0;
        [[nodiscard]] virtual RHIDeviceScopedObjectHandle<RHISwapchain> CreateSwapchain(RHISwapchain::SwapchainDesc const& desc) = 0;
        [[nodiscard]] virtual RHISwapchain* GetSwapchain(Handle handle) const = 0;
        virtual void DestroySwapchain(Handle handle) = 0;

        [[nodiscard]] virtual RHIDeviceScopedObjectHandle<RHIPipelineState> CreatePipelineState(RHIPipelineState::PipelineStateDesc const& desc) = 0;
        [[nodiscard]] virtual RHIPipelineState* GetPipelineState(Handle handle)  const = 0;
        virtual void DestroyPipelineState(Handle handle) = 0;

        [[nodiscard]] virtual RHIDeviceScopedObjectHandle<RHIShaderModule> CreateShaderModule(RHIShaderModule::ShaderModuleDesc const& desc) = 0;
        [[nodiscard]] virtual RHIShaderModule* GetShaderModule(Handle handle) const = 0;
        virtual void DestroyShaderModule(Handle handle) = 0;

        [[nodiscard]] virtual RHIDeviceScopedObjectHandle<RHICommandPool> CreateCommandPool(RHICommandPool::PoolDesc type) = 0;
        [[nodiscard]] virtual RHICommandPool* GetCommandPool(Handle handle) const = 0;
        virtual void DestroyCommandPool(Handle handle) = 0;

        [[nodiscard]] virtual RHIDeviceQueue* GetDeviceQueue(RHIDeviceQueueType type) const = 0;

        [[nodiscard]] virtual RHIDeviceScopedObjectHandle<RHIDeviceSemaphore> CreateSemaphore(bool is_timeline = false) = 0;
        [[nodiscard]] virtual RHIDeviceSemaphore* GetSemaphore(Handle handle) const = 0;
        virtual void DestroySemaphore(Handle handle) = 0;

        [[nodiscard]] virtual RHIDeviceScopedObjectHandle<RHIDeviceFence> CreateFence(bool signaled = true) = 0;
        [[nodiscard]] virtual RHIDeviceFence* GetFence(Handle handle) const = 0;
        virtual void DestroyFence(Handle handle) = 0;

        [[nodiscard]] virtual RHIDeviceScopedObjectHandle<RHIBuffer> CreateBuffer(RHIBufferDesc const& desc) = 0;
        [[nodiscard]] virtual RHIBuffer* GetBuffer(Handle handle) const = 0;
        virtual void DestroyBuffer(Handle handle) = 0;

        [[nodiscard]] virtual RHIDeviceScopedObjectHandle<RHITexture> CreateTexture(RHITextureDesc const& desc) = 0;
        [[nodiscard]] virtual RHITexture* GetImage(Handle handle) const = 0;
        virtual void DestroyImage(Handle handle) = 0;

        [[nodiscard]] virtual RHIDeviceScopedObjectHandle<RHIDeviceDescriptorSetLayout> CreateDescriptorSetLayout(RHIDeviceDescriptorSetLayoutDesc const& desc) = 0;
        [[nodiscard]] virtual RHIDeviceDescriptorSetLayout* GetDescriptorSetLayout(Handle handle) const = 0;
        virtual void DestroyDescriptorSetLayout(Handle handle) = 0;

        [[nodiscard]] virtual RHIDeviceScopedObjectHandle<RHIDeviceDescriptorPool> CreateDescriptorPool(
            RHIDeviceDescriptorPool::PoolDesc const& desc) = 0;
        [[nodiscard]] virtual RHIDeviceDescriptorPool* GetDescriptorPool(Handle handle) const = 0;
        virtual void DestroyDescriptorPool(Handle handle) = 0;

        [[nodiscard]] virtual RHIDeviceScopedObjectHandle<RHIDeviceSampler> CreateSampler(
            RHIDeviceSampler::SamplerDesc const& desc) = 0;
        [[nodiscard]] virtual RHIDeviceSampler* GetSampler(Handle handle) const = 0;
        virtual void DestroySampler(Handle handle) = 0;

        virtual void ResetFences(Span<RHIDeviceFence* const> fences) = 0;
        /**
         * @brief Wait for fences to arrive.
         * @param timeout Wait timeout in nanoseconds. Set to 0 for no wait and return immediately, -1 for infinite wait.
         * @return true if fence reached, false if timeout occurred.
         */
        virtual bool WaitForFences(Span<RHIDeviceFence* const> fences, bool wait_all, size_t timeout) = 0;

        virtual void SignalTimelineSemaphores(Span<const Pair<RHIDeviceSemaphore*, size_t>> semaphores) = 0;
        /**
         * @brief Wait for timeline semaphores to reach specified values.
         * @param timeout Wait timeout in nanoseconds. Set to 0 for no wait and return immediately, -1 for infinite wait.
         * @return true if all semaphores reached the specified values, false if timeout occurred.
         */
        virtual bool WaitForTimelineSemaphores(Span<const Pair<RHIDeviceSemaphore*, size_t>> semaphores, size_t timeout) = 0;

        virtual void WaitIdle() const = 0;

        virtual void DebugSetObjectName(const char* name) = 0;
    };
    /**
     * @brief RAII guard to wait for device idle on destruction.
     */
    struct RHIDeviceIdleGuard
    {
        RHIDevice const* mDevice { nullptr };
        RHIDeviceIdleGuard() = delete;
        RHIDeviceIdleGuard(RHIDevice const* device): mDevice(device) {};
        void WaitIdle() const
        {
            if (mDevice) mDevice->WaitIdle();
        }
        ~RHIDeviceIdleGuard() {
            WaitIdle();
        }

    };

    template<> struct RHIObjectTraits<RHIDevice, RHISwapchain> {
        static RHISwapchain* Get(RHIDevice const* device, Handle handle) {
            return device->GetSwapchain(handle);
        }
        static void Destroy(RHIDevice* device, Handle handle) {
            device->DestroySwapchain(handle);
        }
    };
    template<> struct RHIObjectTraits<RHIDevice, RHIPipelineState> {
        static RHIPipelineState* Get(RHIDevice const* device, Handle handle) {
            return device->GetPipelineState(handle);
        }
        static void Destroy(RHIDevice* device, Handle handle) {
            device->DestroyPipelineState(handle);
        }
    };
    template<> struct RHIObjectTraits<RHIDevice, RHIShaderModule> {
        static RHIShaderModule* Get(RHIDevice const* device, Handle handle) {
            return device->GetShaderModule(handle);
        }
        static void Destroy(RHIDevice* device, Handle handle) {
            device->DestroyShaderModule(handle);
        }
    };
    template<> struct RHIObjectTraits<RHIDevice, RHICommandPool> {
        static RHICommandPool* Get(RHIDevice const* device, Handle handle) {
            return device->GetCommandPool(handle);
        }
        static void Destroy(RHIDevice* device, Handle handle) {
            device->DestroyCommandPool(handle);
        }
    };
    template<> struct RHIObjectTraits<RHIDevice, RHIDeviceSemaphore> {
        static RHIDeviceSemaphore* Get(RHIDevice const* device, Handle handle) {
            return device->GetSemaphore(handle);
        }
        static void Destroy(RHIDevice* device, Handle handle) {
            device->DestroySemaphore(handle);
        }
    };
    template<> struct RHIObjectTraits<RHIDevice, RHIDeviceFence> {
        static RHIDeviceFence* Get(RHIDevice const* device, Handle handle) {
            return device->GetFence(handle);
        }
        static void Destroy(RHIDevice* device, Handle handle) {
            device->DestroyFence(handle);
        }
    };
    template<> struct RHIObjectTraits<RHIDevice, RHIBuffer> {
        static RHIBuffer* Get(RHIDevice const* device, Handle handle) {
            return device->GetBuffer(handle);
        }
        static void Destroy(RHIDevice* device, Handle handle) {
            device->DestroyBuffer(handle);
        }
    };
    template<> struct RHIObjectTraits<RHIDevice, RHITexture> {
        static RHITexture* Get(RHIDevice const* device, Handle handle) {
            return device->GetImage(handle);
        }
        static void Destroy(RHIDevice* device, Handle handle) {
            device->DestroyImage(handle);
        }
    };
    template<> struct RHIObjectTraits<RHIDevice, RHIDeviceDescriptorSetLayout> {
        static RHIDeviceDescriptorSetLayout* Get(RHIDevice const* device, Handle handle) {
            return device->GetDescriptorSetLayout(handle);
        }
        static void Destroy(RHIDevice* device, Handle handle) {
            device->DestroyDescriptorSetLayout(handle);
        }
    };
    template<> struct RHIObjectTraits<RHIDevice, RHIDeviceDescriptorPool> {
        static RHIDeviceDescriptorPool* Get(RHIDevice const* device, Handle handle) {
            return device->GetDescriptorPool(handle);
        }
        static void Destroy(RHIDevice* device, Handle handle) {
            device->DestroyDescriptorPool(handle);
        }
    };
    template<> struct RHIObjectTraits<RHIDevice, RHIDeviceSampler> {
        static RHIDeviceSampler* Get(RHIDevice const* device, Handle handle) {
            return device->GetSampler(handle);
        }
        static void Destroy(RHIDevice* device, Handle handle) {
            device->DestroySampler(handle);
        }
    };
}
