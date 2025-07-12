#pragma once
#include <Platform/RHI/Details/Details.hpp>
#include <Platform/RHI/PipelineState.hpp>
#include <Platform/RHI/Shader.hpp>
#include <Platform/RHI/Swapchain.hpp>

namespace Foundation {
    namespace Platform {
        namespace RHI {
            class RHIApplication;
            class RHIDevice : public RHIObject {
            protected:
                const RHIApplication& m_app;
            public:
                struct DeviceDesc {
                    uint32_t id;
                    std::string name;
                    // Window to associate with the device, if any.
                    // A Device without a Window is not guaranteed to support swapchains or presentation.
                    Window* window = nullptr;

                    inline DeviceDesc& SetWindow(Window* wnd) {
                        window = wnd;
                        return *this;
                    }
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

                virtual RHIDeviceScopedObjectHandle<RHIShaderPipelineModule> CreateShaderPipelineModule
                (RHIShaderPipelineModule::ShaderPipelineModuleDesc const& desc, RHIDeviceObjectHandle<RHIShaderModule> shader_module) = 0;
                virtual RHIShaderPipelineModule* GetShaderPipelineModule(Handle handle)  const = 0;
                virtual void DestroyShaderPipelineModule(Handle handle) = 0;
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
            template<> struct RHIObjectTraits<RHIShaderPipelineModule> {
                static RHIShaderPipelineModule* Get(RHIDevice const* device, Handle handle) {
                    return device->GetShaderPipelineModule(handle);
                }
                static void Destroy(RHIDevice* device, Handle handle) {
                    device->DestroyShaderPipelineModule(handle);
                }
            };
        }
    }
}
