#pragma once
#include <Core/Core.hpp>
#include <Core/Allocator/DefaultAllocator.hpp>

#include <RHICore/Common.hpp>
#include <RHICore/PipelineState.hpp>
#include <Native/Application.hpp>

#include "Renderer.hpp"
namespace Foundation::Rendering {
    using namespace Foundation::Core;
    struct ApplicationInitDesc {
        const size_t deviceIndex{ 0 };
        const char* windowTitle{ "Application" };
        RHIExtent2D windowSize{ 1280, 720 };
        bool present{ true };
        bool asyncCompute{ true };
    };
    /// <summary>
    /// Lightweight template for a rendering application
    /// </summary>
    class Application {
    protected:
        ApplicationInitDesc const& m_desc;

        DefaultAllocator m_alloc, m_alloc_renderer;

        Native::Application m_app;
        Native::Window m_window;

        UniquePtr<RHIApplication> m_rhi;

        RHIApplicationScopedObjectHandle<RHIDevice> m_device;
        RHIDeviceScopedObjectHandle<RHISwapchain> m_swapchain;

        UniquePtr<Renderer> m_renderer;

        inline const RHIExtent2D GetWindowSize() { auto [w, h] = m_window.GetSize(); return { w, h }; }

        void CreateSwapchain();
        void InitializeInternal();
        void InitializeRenderer();
        /// <summary>
        /// Setup the renderer by creating passes, resources, and other configurations.
        ///
        /// This is invoked by InitializeRenderer()
        /// within a Renderer::BeginSetup() and Renderer::EndSetup() clause.        
        ///
        /// Invoking BeginSetup, EndSetup here again is incorrect.
        ///
        /// Implementation that leave this empty will result in a warning log, and
        /// no graphics work will be done.
        ///
        /// This should not be directly called.      
        /// </summary>
        virtual void RendererSetup() {
            LOG_RUNTIME(GraphicsApplication, warn, "RendererSetup() not implemented!");
        }
        /// <summary>
        /// Action to take when the swapchain is resized e.g.
        /// resize resources that depend on the swapchain size.
        ///
        /// Implementation may leave this empty if no action is needed.
        /// </summary>
        virtual void OnSwapchainResize() { InitializeRenderer(); }
        ~Application();
    public:
        Application(ApplicationInitDesc const& desc = {}) : m_desc(desc) {};
        /// <summary>
        /// Initialize the application with the specified RHI backend.
        ///
        /// This must be called before RunForever().
        /// </summary>        
        template<typename Backend, typename... Args>
        void Initialize(Args&&... args) {
            // XXX: Backends are expected to take Allocator* as the first argument
            m_rhi = ConstructUniqueBase<RHIApplication, Backend>(m_alloc.Ptr(), m_alloc.Ptr(), std::forward<Args>(args)...);
            InitializeInternal();
            InitializeRenderer();
        }
        /* --- */
        inline Renderer*  GetRenderer() { return m_renderer.get(); }
        inline Allocator* GetAllocator() { return m_alloc.Ptr(); }
        inline Allocator* GetRendererAllocator() { return m_alloc_renderer.Ptr(); }
        /// <summary>
        /// Run the main loop of the application.
        ///
        /// Swapchain resize, etc, is handled automatically.
        /// </summary>
        void RunForever();
    };
}
