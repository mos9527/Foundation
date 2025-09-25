#pragma once
#include <Core/Core.hpp>
#include <Core/DefaultAllocator.hpp>

#include <Async/Future.hpp>
#include <Async/Thread.hpp>
#include <Native/Application.hpp>

#include <RHICore/Common.hpp>
#include <RHICore/Application.hpp>
#include <RenderCore/Renderer.hpp>

#include <Atomics/Atomic.hpp>
/**
 * @brief Reference implementations of real-time rendering routines.
 */
namespace Foundation::Rendering {
    using namespace Core;
    using namespace RenderCore;
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
        /**
         * @brief Enable async compute support.
         *
         * This may improve performance if you have a GPU that
         * supports async compute well, and your render graph
         * has a good (and trivial) amount of compute work to do.
         *
         * Do note that this may introduce additional latency due
         * to synchronization overhead, and cause performance regression
         * if your render graph doesn't have enough compute work to
         * balance it out.
         */
        bool asyncCompute{ true };
        /**
        * @brief Enable VSync when presenting.
        */
        bool vsync{false};
    };
    /**
    *  @brief Template base class for rendering applications.        
    */
    class RenderApplication : public Native::NativeApplication {
        void Execute();
        void CreateSwapchain();
        void InitializeInternal();
        void InitializeRenderer();
        void RenderWorker();        
    public:
        /**
         * @brief Frame timing information for performance measurements.
         *
         * This is updated every frame in RenderWorker(), where `delta` reflects
         * a rolling average over the last @ref FrameTiming::kTimingSampleDuration duration.
         */
        struct FrameTiming
        {
            // [Frame Number, Perf Counter [ns]]
            using FTick = glm::vec<2, size_t>;
            // States
            FTick begin, delta;
            FTick Tick(FTick const& t)
            {
                delta = t - begin;
                begin = t;
                return delta;
            }
            // Returns the average FPS over the last sample duration.
            [[nodiscard]] size_t GetFPS() const
            {
                if (delta.y == 0) return 0;
                return static_cast<size_t>(1e9 * delta.x / delta.y);
            }
            const size_t kTimingSampleDuration{ static_cast<size_t>(1.0 * 1e9) /* 1 second */ };
        };
    protected:
        ApplicationInitDesc mDesc;

        DefaultAllocator mAlloc, mAllocRenderer;
        Native::NativeWindow mWindow;
        UniquePtr<RHIApplication> mRhi;

        RHIApplicationScopedObjectHandle<RHIDevice> mDevice;
        RHIDeviceScopedObjectHandle<RHISwapchain> mSwapchain;

        UniquePtr<Renderer> mRenderer;
        FrameTiming mTiming{};

        Async::Thread mRenderThread;
        Async::Condition mRenderFrame;
        Async::Mutex mRenderMutex;

        // Should the Render thread reset the renderer on the next frame?
        Atomics::Atomic<bool> mRenderThreadReset{false};
        // Should the application exit?
        Atomics::Atomic<bool> mAppShouldClose{false};
        /**
         * @brief Actions to take after device specific resources has been set up.
         *
         * This is run on the main thread, i.e. the calling thread of @ref Initialize(),
         * after the device and swapchain is created, but before the Renderer is created.
         *
         * Implementation may leave this empty if no action is needed.
         *
         * @note This should not be directly called.
         */
        virtual void OnDeviceSetup() { /* nop */ }
        /**
         * @brief Action to take when the swapchain is resized e.g.
         * resize resources that depend on the swapchain size, and before the
         * @ref Renderer is reinstantiated with the new swapchain.
         *
         * This is run on the Render thread.
         *
         * Implementation may leave this empty if no action is needed.
         *
         * @note This should not be directly called.
         */
        virtual void OnSwapchainResize() { /* nop */ }
        /**
         * @brief Action to take before each frame is executed.
         *
         * This is run on the Render thread.
         *
         * This is invoked before the renderer executes the passes,
         * but after the current frame is acquired from the swapchain.
         *
         * Implementation may leave this empty if no action is needed.
         *
         * @note This should not be directly called.
         */
        virtual void OnBeforeFrame() { /* nop */ }
        /**
         * @brief Action to take after each frame is executed.
         *
         * This is run on the Render thread.
         *
         * This is invoked after the renderer has submitted all passes,
         * but before the next frame could be rendered.
         *
         * Implementation may leave this empty if no action is needed.
         *
         * @note This should not be directly called.
         */
        virtual void OnAfterFrame() { /* nop */ }
        /**
         * @brief Set up the renderer by creating passes, resources, and other configurations.
         *
         * @note This is where you should set up your rendering pipeline.
         *
         * @note This is invoked by @ref InitializeRenderer on the Render thread,
         *       within a @ref Renderer::BeginSetup and @ref Renderer::EndSetup clause,
         *       thus invoking @ref Renderer::BeginSetup, @ref Renderer::EndSetup here again is incorrect.
         *       and will be called again if the swapchain is recreated.
         *
         * @note This MUST be implemented by the subclass.
         *
         * @note This should not be directly called.
         */
        virtual void OnRendererSetup() = 0;
        /**
         * @brief Action to take on every application tick.
         *
         * @note This is run on the main thread, i.e. the calling thread of @ref RunForever.
         *       No synchronization is performed on a per-frame basis. You may want to
         *       use @ref WaitForFrame() to synchronize with the render thread if needed.
         *
         * @note This runs in a tight loop in @ref RunForever()
         *
         * Implementation may leave this empty if no action is needed.
         *
         * @note This should not be directly called.
         */
        virtual void OnApplicationTick() { /* nop */ }
    public:
        /**
         * @brief Initialize the application with the specified RHI backend.
         *
         * @note This must be called before RunForever().
         */
        template<typename Backend, typename... Args>
        void Initialize(ApplicationInitDesc const& desc = {}, Args&&... args) {
            // XXX: Backends are expected to take Allocator* as the first argument
            mDesc = desc;
            mRhi.reset();
            mRhi = ConstructUniqueBase<RHIApplication, Backend>(mAlloc.Ptr(), mAlloc.Ptr(),
                                                                 std::forward<Args>(args)...);
            InitializeInternal();
            InitializeRenderer();
        }
        /**
         * @brief Retrieve the framebuffer size of the current window.
         */
        [[nodiscard]] RHIExtent2D GetFramebufferSize() const { auto [w, h] = mWindow.GetFramebufferSize(); return { w, h }; }
        /**
         * @brief Retrieve the timing information of the last frame.
         */
        [[nodiscard]] FrameTiming GetTiming() const { return mTiming; }
        /**
         * @brief Retrieve the underlying Renderer instance.
         */
        [[nodiscard]] Renderer* GetRenderer() const { return mRenderer.get(); }
        /**
         * @brief Retrieve the current RHISwapchain instance.
         */
        [[nodiscard]] RHISwapchain* GetSwapchain() const
        {
            CHECK_MSG(mSwapchain, "No swapchain created. Did you initialize the application with present=true?");
            return mSwapchain.Get();
        }
        /**
         * @brief Retrieve the current NativeWindow instance.
         */
        Native::NativeWindow* GetNativeWindow() { return &mWindow; }
        /**
         * @brief Retrieve the allocator used for general application allocations.
         */
        Allocator* GetAllocator() { return mAlloc.Ptr(); }
        /**
         * @brief Retrieve the allocator used for renderer allocations.
         */
        Allocator* GetRendererAllocator() { return mAllocRenderer.Ptr(); }
        /**
         * @brief Start the Render thread and run the application loop indefinitely,
         * until the window is closed or the application is exited.
         */
        void RunForever();
        /**
         * @brief Wait for the next frame to be rendered.
         */
        void WaitForFrame();
        /**
         * @brief Reset the renderer on the next frame, calling @ref OnRendererSetup internally.         
         * @note This can be called from any thread, and will be executed on the Render thread on the next frame of
         *       its work.
         */
        void ResetRendererOnNextFrame();
        /**
         * @brief Flag the application to exit.
         * @note This can be called from any thread.
         * @note This does not immediately terminate the application. At the end of the current
         *       tick of the main loop in @ref RunForever(), the application will exit gracefully if
         *       this is called, or the main window is closed.
         */           
        void Shutdown();
    };
}
