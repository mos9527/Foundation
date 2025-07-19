#pragma once
#include <Platform/RHI/PipelineState.hpp>
#include <Platform/RHI/Shader.hpp>
#include <Platform/RHI/Swapchain.hpp>
#include <Platform/RHI/Command.hpp>
#include <Platform/RHI/Resource.hpp>
#include <Platform/RHI/Descriptor.hpp>
namespace Foundation {
    namespace Platform {
        namespace RHI {
            class RHIApplication;            
            class RHIDevice;
            class RHIDeviceSemaphore;
            class RHIDeviceFence;
            class RHIDeviceQueue : public RHIObject {
            protected:
                const RHIDevice& m_device;
            public:
                RHIDeviceQueue(RHIDevice const& device) : m_device(device) {}

                virtual void WaitIdle() const = 0;

                struct SubmitDesc {
                    RHIPipelineStage stages;
                    Core::StlSpan<const RHIDeviceObjectHandle<RHIDeviceSemaphore>> waits, signals;                    
                    Core::StlSpan<const RHICommandPoolHandle<RHICommandList>> cmd_lists;
                    RHIDeviceObjectHandle<RHIDeviceFence> fence;
                };
                virtual void Submit(SubmitDesc const& desc) const = 0;

                struct PresentDesc {
                    uint32_t image_index;
                    RHIDeviceObjectHandle<RHISwapchain> swapchain;
                    Core::StlSpan<const RHIDeviceObjectHandle<RHIDeviceSemaphore>> waits;
                };
                virtual void Present(PresentDesc const& desc) const = 0;
            };
            class RHIDeviceSemaphore : public RHIObject {
            protected:
                const RHIDevice& m_device;
            public:
                RHIDeviceSemaphore(RHIDevice const& device) : m_device(device) {}
            };
            class RHIDeviceFence : public RHIObject {
            protected:
                const RHIDevice& m_device;
            public:
                RHIDeviceFence(RHIDevice const& device) : m_device(device) {}
            };
            struct RHIDeviceDescriptorSetLayoutDesc {
                struct Binding {
                    uint32_t count{ 1 }; // Array size for array access
                    RHIShaderStage stage{ RHIShaderStage::All }; // Stage this binding is used in
                    RHIDescriptorType type; // Type of this binding
                };
                Core::StlSpan<const Binding> bindings;
            };
            class RHIDeviceDescriptorSetLayout : public RHIObject {
            protected:
                const RHIDevice& m_device;
            public:
                const RHIDeviceDescriptorSetLayoutDesc m_desc;
                RHIDeviceDescriptorSetLayout(RHIDevice const& device, RHIDeviceDescriptorSetLayoutDesc const& desc)
                    : m_device(device), m_desc(desc) {
                }
            };
            class RHIDeviceSampler : public RHIObject {
            protected:
                const RHIDevice& m_device;
            public:
                struct SamplerDesc {
                    struct Anisotropy {
                        bool enable{ false }; // Enable anisotropic filtering
                        float max_level{ 16.0f }; // Max anisotropy level
                    } anisotropy;                    
                    struct AddressMode {
                        enum Mode {
                            Repeat,
                            MirroredRepeat,
                            ClampToEdge,
                            ClampToBorder,
                            MirrorClampToEdge
                        } u{ Repeat }, v{ Repeat }, w{ Repeat }; // Address modes for U, V, W coordinates
                    } address_mode;
                    struct Mipmap {
                        enum MipmapMode {
                            Linear,
                            Nearest
                        } mipmap_mode{ Linear }; // Mipmap mode;
                        float bias{ 0.0f }; // Mipmap LOD bias
                    } mipmap;
                    struct Filter {
                        enum Type {
                            NearestNeighbor,
                            Linear,
                            Cubic
                        } min_filter{ Linear }, mag_filter{ Linear }; // Minification and magnification filters
                    } filter;
                    struct LOD {
                        float min{ 0.0f }; // Minimum level of detail
                        float max{ 16.0f }; // Maximum level of detail                        
                    } lod; // Level of detail settings;
                } const m_desc;
                RHIDeviceSampler(RHIDevice const& device, SamplerDesc const& desc)
                    : m_device(device), m_desc(desc) {}
            };
            class RHIDevice : public RHIObject {
            protected:
                const RHIApplication& m_app;
            public:
                struct DeviceDesc {
                    uint32_t id;
                    std::string name;
                };
                RHIDevice(RHIApplication const& app) : m_app(app) {}
                RHIDevice(RHIDevice const&) = delete;

                virtual RHIDeviceScopedObjectHandle<RHISwapchain> CreateSwapchain(RHISwapchain::SwapchainDesc const& desc) = 0;
                virtual RHISwapchain* GetSwapchain(Handle handle) const = 0;
                virtual void DestroySwapchain(Handle handle) = 0;

                virtual RHIDeviceScopedObjectHandle<RHIPipelineState> CreatePipelineState(RHIPipelineState::PipelineStateDesc const& desc) = 0;
                virtual RHIPipelineState* GetPipelineState(Handle handle)  const = 0;
                virtual void DestroyPipelineState(Handle handle) = 0;

                virtual RHIDeviceScopedObjectHandle<RHIShaderModule> CreateShaderModule(RHIShaderModule::ShaderModuleDesc const& desc) = 0;
                virtual RHIShaderModule* GetShaderModule(Handle handle) const = 0;
                virtual void DestroyShaderModule(Handle handle) = 0;

                virtual RHIDeviceScopedObjectHandle<RHICommandPool> CreateCommandPool(RHICommandPool::PoolDesc type) = 0;
                virtual RHICommandPool* GetCommandPool(Handle handle) const = 0;
                virtual void DestroyCommandPool(Handle handle) = 0;

                virtual RHIDeviceQueue* GetDeviceQueue(RHIDeviceQueueType type) const = 0;

                virtual RHIDeviceScopedObjectHandle<RHIDeviceSemaphore> CreateSemaphore() = 0;
                virtual RHIDeviceSemaphore* GetSemaphore(Handle handle) const = 0;
                virtual void DestroySemaphore(Handle handle) = 0;

                virtual RHIDeviceScopedObjectHandle<RHIDeviceFence> CreateFence(bool signaled = true) = 0;
                virtual RHIDeviceFence* GetFence(Handle handle) const = 0;
                virtual void DestroyFence(Handle handle) = 0;

                virtual RHIDeviceScopedObjectHandle<RHIBuffer> CreateBuffer(RHIBufferDesc const& desc) = 0;
                virtual RHIBuffer* GetBuffer(Handle handle) const = 0;
                virtual void DestroyBuffer(Handle handle) = 0;

                virtual RHIDeviceScopedObjectHandle<RHIImage> CreateImage(RHIImageDesc const& desc) = 0;
                virtual RHIImage* GetImage(Handle handle) const = 0;
                virtual void DestroyImage(Handle handle) = 0;

                virtual RHIDeviceScopedObjectHandle<RHIDeviceDescriptorSetLayout> CreateDescriptorSetLayout(RHIDeviceDescriptorSetLayoutDesc const& desc) = 0;
                virtual RHIDeviceDescriptorSetLayout* GetDescriptorSetLayout(Handle handle) const = 0;
                virtual void DestroyDescriptorSetLayout(Handle handle) = 0;

                virtual RHIDeviceScopedObjectHandle<RHIDeviceDescriptorPool> CreateDescriptorPool(
                    RHIDeviceDescriptorPool::PoolDesc const& desc) = 0;
                virtual RHIDeviceDescriptorPool* GetDescriptorPool(Handle handle) const = 0;
                virtual void DestroyDescriptorPool(Handle handle) = 0;

                virtual RHIDeviceScopedObjectHandle<RHIDeviceSampler> CreateSampler(
                    RHIDeviceSampler::SamplerDesc const& desc) = 0;
                virtual RHIDeviceSampler* GetSampler(Handle handle) const = 0;
                virtual void DestroySampler(Handle handle) = 0;

                virtual void ResetFences(Core::StlSpan<const RHIDeviceObjectHandle<RHIDeviceFence>> fences) = 0;
                virtual void WaitForFences(Core::StlSpan<const RHIDeviceObjectHandle<RHIDeviceFence>> fences, bool wait_all, size_t timeout) = 0;
            };

            template<> struct RHIObjectTraits<RHISwapchain> {
                static RHISwapchain* Get(RHIDevice const* device, Handle handle) {
                    return device->GetSwapchain(handle);
                }
                static void Destroy(RHIDevice* device, Handle handle) {
                    device->DestroySwapchain(handle);
                }
            };
            template<> struct RHIObjectTraits<RHIPipelineState> {
                static RHIPipelineState* Get(RHIDevice const* device, Handle handle) {
                    return device->GetPipelineState(handle);
                }
                static void Destroy(RHIDevice* device, Handle handle) {
                    device->DestroyPipelineState(handle);
                }
            };
            template<> struct RHIObjectTraits<RHIShaderModule> {
                static RHIShaderModule* Get(RHIDevice const* device, Handle handle) {
                    return device->GetShaderModule(handle);
                }
                static void Destroy(RHIDevice* device, Handle handle) {
                    device->DestroyShaderModule(handle);
                }
            };
            template<> struct RHIObjectTraits<RHICommandPool> {
                static RHICommandPool* Get(RHIDevice const* device, Handle handle) {
                    return device->GetCommandPool(handle);
                }
                static void Destroy(RHIDevice* device, Handle handle) {
                    device->DestroyCommandPool(handle);
                }
            };
            template<> struct RHIObjectTraits<RHIDeviceSemaphore> {
                static RHIDeviceSemaphore* Get(RHIDevice const* device, Handle handle) {
                    return device->GetSemaphore(handle);
                }
                static void Destroy(RHIDevice* device, Handle handle) {
                    device->DestroySemaphore(handle);
                }
            };
            template<> struct RHIObjectTraits<RHIDeviceFence> {
                static RHIDeviceFence* Get(RHIDevice const* device, Handle handle) {
                    return device->GetFence(handle);
                }
                static void Destroy(RHIDevice* device, Handle handle) {
                    device->DestroyFence(handle);
                }
            };
            template<> struct RHIObjectTraits<RHIBuffer> {
                static RHIBuffer* Get(RHIDevice const* device, Handle handle) {
                    return device->GetBuffer(handle);
                }
                static void Destroy(RHIDevice* device, Handle handle) {
                    device->DestroyBuffer(handle);
                }
            };
            template<> struct RHIObjectTraits<RHIImage> {
                static RHIImage* Get(RHIDevice const* device, Handle handle) {
                    return device->GetImage(handle);
                }
                static void Destroy(RHIDevice* device, Handle handle) {
                    device->DestroyImage(handle);
                }
            };
            template<> struct RHIObjectTraits<RHIDeviceDescriptorSetLayout> {
                static RHIDeviceDescriptorSetLayout* Get(RHIDevice const* device, Handle handle) {
                    return device->GetDescriptorSetLayout(handle);
                }
                static void Destroy(RHIDevice* device, Handle handle) {
                    device->DestroyDescriptorSetLayout(handle);
                }
            };
            template<> struct RHIObjectTraits<RHIDeviceDescriptorPool> {
                static RHIDeviceDescriptorPool* Get(RHIDevice const* device, Handle handle) {
                    return device->GetDescriptorPool(handle);
                }
                static void Destroy(RHIDevice* device, Handle handle) {
                    device->DestroyDescriptorPool(handle);
                }
            };
            template<> struct RHIObjectTraits<RHIDeviceSampler> {
                static RHIDeviceSampler* Get(RHIDevice const* device, Handle handle) {
                    return device->GetSampler(handle);
                }
                static void Destroy(RHIDevice* device, Handle handle) {
                    device->DestroySampler(handle);
                }
            };
        }
    }
}
