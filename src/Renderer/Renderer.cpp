#include <array>
#include <fstream>
#include <filesystem>


#include <Math/Math.hpp>
#include <Core/Platform/Logging.hpp>
#include <RHICore/Device.hpp>

#include "Renderer.hpp"

#include <Cooking/Image.hpp>
#include <Cooking/Mesh.hpp>
using namespace Foundation;
using namespace Foundation::Core;

void Renderer::CreateSwapchain(RHIExtent2D size) {
    m_gfxQueue->WaitIdle();
    if (m_swapchain)
        m_swapchain.Reset();
    m_swapchain = m_device->CreateSwapchain(RHISwapchain::SwapchainDesc{
        .format = RHIResourceFormat::R8G8B8A8_UNORM,
        .dimensions = size,
        .buffer_count = 3,
        .present_mode = RHISwapchain::SwapchainDesc::PresentMode::MAILBOX,
    });
}
Renderer::Renderer(RHIApplicationObjectHandle<RHIDevice> device, RHIExtent2D initialSize, Core::Allocator* allocator)
    : m_device(device), m_allocator(allocator), m_renderPasses(allocator), m_resourceDefines(allocator) {
    m_gfxQueue = m_device->GetDeviceQueue(RHIDeviceQueueType::Graphics);
    m_compQueue = m_device->GetDeviceQueue(RHIDeviceQueueType::Compute);
    m_cmdPool = m_device->CreateCommandPool(RHICommandPool::PoolDesc{
        .queue = RHIDeviceQueueType::Graphics,
        .type = RHICommandPoolType::Persistent
    });
    CreateSwapchain(initialSize);
}

void Renderer::Draw(RHIExtent2D currentSize) {
    // Handle resize
    if (currentSize != m_swapchain->GetDimensions())    
        CreateSwapchain(currentSize);   
}

void Renderer::BeginSetup() {
    m_setupContext.reset();
    m_resourceDefines.clear();
    m_renderPasses.clear();
}

void Renderer::DeclareAccess(ResourceHandle handle, ResourceAccess access) {
    CHECK(m_setupContext && "Setup context not initialized. Did you call EndSetup()?");
    auto& resource = m_resourceDefines[handle];
    if (resource.lastProducerPass.has_value()) {

        m_setupContext->add_edge(m_setupContext->currentPass, resource.lastProducerPass.value(), handle);
    }
    if (access == ResourceAccess::Write || access == ResourceAccess::ReadWrite) {
        resource.lastProducerPass = m_setupContext->currentPass;
    }
}

void Renderer::EndSetup() {
    m_setupContext = ConstructUnique<SetupContext>(m_allocator, m_allocator);
    for (size_t i = 0; i < m_renderPasses.size(); ++i) {
        m_setupContext->currentPass = i;
        m_renderPasses[i].pass->Setup(*this);
    }
}
