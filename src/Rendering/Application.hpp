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
        size_t deviceIndex{ 0 };
        std::string windowTitle{"Application"};
        RHIExtent2D windowSize{ 1280, 720 };
        bool present{ true };
        bool asyncCompute{ true };
    };
    /**
     * @brief Lightweight template for a rendering application
     */
    class Application : public Native::Application {
    protected:
        ApplicationInitDesc m_desc;

        DefaultAllocator m_alloc, m_alloc_renderer;
        Native::Window m_window;
        UniquePtr<RHIApplication> m_rhi;

        RHIApplicationScopedObjectHandle<RHIDevice> m_device;
        RHIDeviceScopedObjectHandle<RHISwapchain> m_swapchain;

        UniquePtr<Renderer> m_renderer;

        inline const RHIExtent2D GetWindowSize() { auto [w, h] = m_window.GetSize(); return { w, h }; }

        void CreateSwapchain();
        void InitializeInternal();
        void InitializeRenderer();
        /**
         * @brief Setup the renderer by creating passes, resources, and other configurations.
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
        ~Application();
    public:
        Application() : Native::Application() {};
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
        inline Renderer*  GetRenderer() { return m_renderer.get(); }
        inline Allocator* GetAllocator() { return m_alloc.Ptr(); }
        inline Allocator* GetRendererAllocator() { return m_alloc_renderer.Ptr(); }
        /**
         * @brief Run the main loop of the application.
         *
         * Swapchain resize, etc, is handled automatically.
         */
        void RunForever();
    };
}
