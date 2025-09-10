#pragma once
#include <Core/Core.hpp>
#include <Core/Allocator/DefaultAllocator.hpp>

#include <RHICore/Common.hpp>
#include <Native/Application.hpp>

#include "Renderer.hpp"
namespace Foundation::Rendering {
    using namespace Foundation::Core;
    /**
     * @brief Initialization parameters for RenderApplication.
     */
    struct ApplicationInitDesc {
        size_t deviceIndex{ 0 };
        String windowTitle{"Application"};
        RHIExtent2D windowSize{ 800, 600 };
        /**
         * @brief Enable Present support.
         *
         * Disable this if you want to do headless work e.g.
         * rendering to offscreen textures, etc.
         */
        bool present{ true };
        bool asyncCompute{ true }; // Enable async compute queue if available
    };
    /**
    *  @brief Template base class for rendering applications.        
    */
    class RenderApplication : public Native::NativeApplication {
    protected:
        ApplicationInitDesc m_desc;

        DefaultAllocator m_alloc, m_alloc_renderer;
        Native::NativeWindow m_window;
        UniquePtr<RHIApplication> m_rhi;

        RHIApplicationScopedObjectHandle<RHIDevice> m_device;
        RHIDeviceScopedObjectHandle<RHISwapchain> m_swapchain;

        UniquePtr<Renderer> m_renderer;

        RHIExtent2D GetFramebufferSize() const
        { auto [w, h] = m_window.GetFramebufferSize(); return { w, h }; }

        void CreateSwapchain();
        void InitializeInternal();
        void InitializeRenderer();
        /**
         * @brief Set up the renderer by creating passes, resources, and other configurations.
         *
         * This is invoked by InitializeRenderer()
         * within a Renderer::BeginSetup() and Renderer::EndSetup() clause.        
         *
         * Invoking BeginSetup, EndSetup here again is incorrect.
         *
         * Implementation that leave this empty will result in a warning log, and
         * no graphics work will be done.
         *
         * This should not be directly called.      
         */
        virtual void RendererSetup() {
            LOG_RUNTIME(GraphicsApplication, warn, "RendererSetup() not implemented!");
        }
        /**
         * @brief Action to take when the swapchain is resized e.g.
         * resize resources that depend on the swapchain size.
         *
         * Implementation may leave this empty if no action is needed.
         */
        virtual void OnSwapchainResize() { InitializeRenderer(); }
        /**
         * @brief Action to take before each frame is executed.
         *
         * This is invoked by Execute() before the renderer is executed.
         *
         * Implementation may leave this empty if no action is needed.
         */
        virtual void OnBeforeFrame() { /* nop */ }
        /**
         * @brief Execute one frame of the application.
         *
         * This is invoked by RunForever() every frame, and should not be directly called.
         */
        void Execute();
    public:
        /**
         * @brief Initialize the application with the specified RHI backend.
         *
         * This must be called before RunForever().
         */
        template<typename Backend, typename... Args>
        void Initialize(ApplicationInitDesc const& desc = {}, Args&&... args) {
            // XXX: Backends are expected to take Allocator* as the first argument
            m_desc = desc;
            m_rhi.reset();
            m_rhi = ConstructUniqueBase<RHIApplication, Backend>(m_alloc.Ptr(), m_alloc.Ptr(), std::forward<Args>(args)...);
            InitializeInternal();
            InitializeRenderer();
        }
        /* --- */
        Renderer*  GetRenderer() const { return m_renderer.get(); }
        Allocator* GetAllocator() { return m_alloc.Ptr(); }
        Allocator* GetRendererAllocator() { return m_alloc_renderer.Ptr(); }
        /**
         * @brief Run the main loop of the application.
         *
         * Swapchain resize, etc., is handled automatically.
         */
        void RunForever();
    };
}
