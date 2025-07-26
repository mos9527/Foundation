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
StlVector<char> ReadFile(std::filesystem::path const& path, Allocator* allocator) {
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    CHECK(file.good() && "failed to open file");
    StlVector<char> data(allocator);
    data.resize(file.tellg());
    file.seekg(0, std::ios::beg);
    file.read(data.data(), static_cast<std::streamsize>(data.size()));
    file.close();
    return data;
}
struct uniform_buffer {
    // col-major
    glm::mat4 model;
    glm::mat4 view;
    glm::mat4 proj;
};
Renderer::Renderer(RHIApplicationObjectHandle<RHIDevice> device, Core::Allocator* allocator)
    : m_device(device), m_allocator(allocator), m_swaps(allocator), m_desc_set(allocator) {
    // Swapchain & sync primitives setup
    m_swapchain = m_device->CreateSwapchain(RHISwapchain::SwapchainDesc{
        .format = RHIResourceFormat::R8G8B8A8_UNORM,
        .dimensions = {1920, 1080},
        .buffer_count = 3,
        .present_mode = RHISwapchain::SwapchainDesc::PresentMode::MAILBOX,
        });
    m_queue = m_device->GetDeviceQueue(RHIDeviceQueueType::Graphics);
    m_cmd_pool = m_device->CreateCommandPool(RHICommandPool::PoolDesc{
        .queue = RHIDeviceQueueType::Graphics,
        .type = RHICommandPoolType::Persistent
        });
    m_swaps.resize(m_swapchain->GetImages().size());
    for (size_t i = 0; i < m_swaps.size(); ++i) {
        auto& swap = m_swaps[i];
        auto const& image = m_swapchain->GetImages()[i];
        swap.m_sync_present = m_device->CreateSemaphore();
        swap.m_sync_draw = m_device->CreateSemaphore();
        swap.m_fence_draw = m_device->CreateFence(true /* signaled */);
        swap.m_cmd = m_cmd_pool->CreateCommandList();
        swap.m_swapchain_imageview =
            image->CreateTextureView(RHITextureViewDesc{
                .format = RHIResourceFormat::R8G8B8A8_UNORM,
                .range = {
                    .layer = {
                        .mip_level = 0,
                        .base_array_layer = 0,
                        .layer_count = 1
                    },
                    .mip_count = 1
                }
                });
        swap.m_depth = m_device->CreateTexture(RHITextureDesc{
            .resource = {
                .host_access = RHIResourceHostAccess::Invisible,
                .shared = false
            },
            .usage = (RHITextureUsage)(RHITextureUsage::DepthStencil | RHITextureUsage::TransferDestination),
            .extent = { 1920, 1080, 1 },
            .format = RHIResourceFormat::D32_SIGNED_FLOAT,
            .initial_layout = RHITextureLayout::Undefined,
            });
        swap.m_depth_view = swap.m_depth->CreateTextureView(RHITextureViewDesc{
            .format = RHIResourceFormat::D32_SIGNED_FLOAT,
            .range = {
                .layer = {
                    .access = RHITextureAccessFlag::Depth,
                }
            }
            });
    }
    // Loading shaders
    auto shader_vert_data = ReadFile(".derived/shaders/Triangle_vertMain.spirv", m_allocator);
    m_shader_vert = m_device->CreateShaderModule(RHIShaderModule::ShaderModuleDesc{
        .source = shader_vert_data
        });
    auto shader_frag_data = ReadFile(".derived/shaders/Triangle_fragMain.spirv", m_allocator);
    m_shader_frag = m_device->CreateShaderModule(RHIShaderModule::ShaderModuleDesc{
        .source = shader_frag_data
        });
    // Pipeline state setup
    RHIPipelineState::PipelineStateDesc::ShaderStage stages[]{
        {.desc = {
            .stage = RHIShaderStage::Vertex,
            .entry_point = "main"
        }, .shader_module = m_shader_vert },
        {.desc = {
            .stage = RHIShaderStage::Fragment,
            .entry_point = "main"
        }, .shader_module = m_shader_frag }
    };
    m_desc_pool = device->CreateDescriptorPool(RHIDeviceDescriptorPool::PoolDesc{
        .bindings = {{
            {.type = RHIDescriptorType::UniformBuffer, .max_count = 16 },
            {.type = RHIDescriptorType::Sampler, .max_count = 16},
            {.type = RHIDescriptorType::SampledImage, .max_count = 16}
        }}
        });
    m_desc_layout = device->CreateDescriptorSetLayout(RHIDeviceDescriptorSetLayoutDesc{
        .bindings = {{
            {.type = RHIDescriptorType::UniformBuffer },
            {.type = RHIDescriptorType::Sampler },
            {.type = RHIDescriptorType::SampledImage }
        }}
        });
    RHIPipelineState::PipelineStateDesc pipeline{
        .vertex_input = {
            .bindings = {
                {{.stride = sizeof(Cooking::OBJVertex) }}
            },
            .attributes = Cooking::OBJAttributes
        },
        .topology = RHIPipelineState::PipelineStateDesc::Topology::TRIANGLE_LIST,
        .viewport = {.width = 1920, .height = 1080 },
        .scissor = {.width = 1920, .height = 1080 },
        .rasterizer = {
            .fill_mode = RHIPipelineState::PipelineStateDesc::Rasterizer::FILL_SOLID,
            .cull_mode = RHIPipelineState::PipelineStateDesc::Rasterizer::CULL_BACK,
            .front_face = RHIPipelineState::PipelineStateDesc::Rasterizer::FF_COUNTER_CLOCKWISE,
        },
        .multisample = {.enabled = false },
        .depth_stencil = {
            .depth_format = RHIResourceFormat::D32_SIGNED_FLOAT,
            .depth_test = true,
            .depth_write = true
        },
        .attachments = {{
            {
                .blending = {.enabled = false},
                .render_target = {.format = RHIResourceFormat::R8G8B8A8_UNORM }
            }
        }},
        .shader_stages = stages,
        .descriptor_set_layouts = { m_desc_layout },
    };
    m_pso = m_device->CreatePipelineState(pipeline);
    // Buffers    
    auto staging = m_device->CreateBuffer(RHIBufferDesc{
            .resource = {
            .host_access = RHIResourceHostAccess::ReadWrite,
            .coherent = true,
        },
        .usage = RHIBufferUsage::TransferSource,
        .size = 2 * (1 << 20) // 2 MiB
    });
    m_index_buffer = m_device->CreateBuffer(RHIBufferDesc{
        .resource = {
            .host_access = RHIResourceHostAccess::ReadWrite,
        },
        .usage = (RHIBufferUsage)(RHIBufferUsage::IndexBuffer | RHIBufferUsage::TransferDestination),
        .size = 2 * (1 << 20) // 2 MiB
    });
    m_vertex_buffer = m_device->CreateBuffer(RHIBufferDesc{
        .resource = {
            .host_access = RHIResourceHostAccess::ReadWrite,
        },
        .usage = (RHIBufferUsage)(RHIBufferUsage::VertexBuffer | RHIBufferUsage::TransferDestination),
        .size = 2 * (1 << 20) // 2 MiB
    });
    auto copy_buffer = [&](RHIBuffer* dst, const void* src, size_t size) {
        memcpy(staging->Map(), src, size);
        auto cmd = m_cmd_pool->CreateCommandList();
        cmd->Begin();
        cmd->CopyBuffer(
            staging.Get(),
            dst,
            { {RHICommandList::CopyBufferRegion{
                .src_offset = 0,
                .dst_offset = 0,
                .size = size
            }} }
        );
        cmd->End();
        m_queue->Submit(RHIDeviceQueue::SubmitDesc{ .cmd_lists = cmd });
        m_queue->WaitIdle();
    };
    // Meshes
    {
        auto mesh = Cooking::Cook<Blobs::Mesh>::FromOBJ(".derived/kitten.obj", m_allocator);       
        copy_buffer(m_vertex_buffer.Get(), mesh.m_vertex_data.data(), mesh.m_vertex_data.size());
        copy_buffer(m_index_buffer.Get(), mesh.m_index_data.data(), mesh.m_index_data.size());
        m_num_indices = mesh.m_num_indices;
    }
    // Images
    {
        auto image = Cooking::Cook<Blobs::Image>::sRGB32bpp_FromFile(".derived/texture.jpg", m_allocator);
        CHECK(image && "Failed to load texture image");
        m_tex = m_device->CreateTexture(RHITextureDesc{
            .resource = {
                .host_access = RHIResourceHostAccess::Invisible,
            },
            .usage = (RHITextureUsage)(RHITextureUsage::SampledImage | RHITextureUsage::TransferDestination),
            .extent = image.m_desc.extent,
            .format = RHIResourceFormat::R8G8B8A8_UNORM,
            .initial_layout = RHITextureLayout::Undefined,
            });
        m_tex_view = m_tex->CreateTextureView(RHITextureViewDesc{
            .format = RHIResourceFormat::R8G8B8A8_UNORM,
            });
        // Staging again!
        auto staging = m_device->CreateBuffer(RHIBufferDesc{
            .resource = {
                .host_access = RHIResourceHostAccess::ReadWrite,
                .coherent = true,
            },
            .usage = RHIBufferUsage::TransferSource,
            .size = image.m_data.size()
            });
        memcpy(staging->Map(), image.m_data.data(), image.m_data.size());
        auto cmd = m_cmd_pool->CreateCommandList();
        cmd->Begin();
        cmd->BeginTransition();
        cmd->SetImageTransition(
            m_tex.Get(),
            RHICommandList::TransitionDesc{
                .src_access = RHIResourceAccess::Undefined,
                .dst_access = RHIResourceAccess::TransferWrite,
                .src_stage = RHIPipelineStage::TopOfPipe,
                .dst_stage = RHIPipelineStage::Transfer,
                .src_img_layout = RHITextureLayout::Undefined,
                .dst_img_layout = RHITextureLayout::TransferDst
            }
        );
        cmd->EndTransition();
        cmd->CopyBufferToImage(
            staging.Get(),
            m_tex.Get(),
            RHITextureLayout::TransferDst,
            { {RHICommandList::CopyImageRegion{
                .extent = image.m_desc.extent,
            }} }
            );
        cmd->BeginTransition();
        cmd->SetImageTransition(
            m_tex.Get(),
            RHICommandList::TransitionDesc{
                .src_access = RHIResourceAccess::TransferWrite,
                .dst_access = RHIResourceAccess::ShaderRead,
                .src_stage = RHIPipelineStage::Transfer,
                .dst_stage = RHIPipelineStage::FragmentShader,
                .src_img_layout = RHITextureLayout::TransferDst,
                .dst_img_layout = RHITextureLayout::ShaderReadOnly
            }
        );
        cmd->EndTransition();
        cmd->End();
        m_queue->Submit(RHIDeviceQueue::SubmitDesc{
            .cmd_lists = cmd,
            });
        m_queue->WaitIdle();
    }
    m_desc_set.resize(m_swapchain->GetImages().size());
    m_sampler = m_device->CreateSampler(RHIDeviceSampler::SamplerDesc{
        .anisotropy = {
            .enable = true,
            .max_level = 16.0f
        },
        });
    for (size_t i = 0; i < m_desc_set.size(); ++i) {
        auto& set = m_desc_set[i];
        set = m_desc_pool->CreateDescriptorSet(m_desc_layout);
        auto& buffer = m_swaps[i].m_uniform_buffer = m_device->CreateBuffer(RHIBufferDesc{
            .resource = {
                .host_access = RHIResourceHostAccess::ReadWrite,
                .coherent = true
            },
            .usage = (RHIBufferUsage)(RHIBufferUsage::UniformBuffer | RHIBufferUsage::TransferDestination),
            .size = sizeof(uniform_buffer)
            });
        set->Update(RHIDeviceDescriptorSet::UpdateDesc{
            .binding = 0,
            .type = RHIDescriptorType::UniformBuffer,
            .buffers = {{
                {.buffer = buffer.Get(), .offset = 0, .size = sizeof(uniform_buffer) }
            }},
            });
        set->Update(RHIDeviceDescriptorSet::UpdateDesc{
            .binding = 1,
            .type = RHIDescriptorType::Sampler,
            .images = {{
                {.sampler = m_sampler.Get(), }
            }}
            });
        set->Update(RHIDeviceDescriptorSet::UpdateDesc{
            .binding = 2,
            .type = RHIDescriptorType::SampledImage,
            .images = {{
                {.image_view = m_tex_view.Get(), .layout = RHITextureLayout::ShaderReadOnly }
            }}
            });
    }
}
void Renderer::Record(uint32_t image_index, RHICommandList* cmd) {
    cmd->Begin();
    cmd->BeginTransition();
    cmd->SetImageTransition(
        m_swapchain->GetImages()[image_index],
        RHICommandList::TransitionDesc{
            .src_access = RHIResourceAccess::Undefined,
            .dst_access = RHIResourceAccess::RenderTargetWrite,
            .src_stage = RHIPipelineStage::TopOfPipe,
            .dst_stage = RHIPipelineStage::RenderTargetOutput,
            .src_img_layout = RHITextureLayout::Undefined,
            .dst_img_layout = RHITextureLayout::RenderTarget
        }
    );
    cmd->SetImageTransition(
        m_swaps[image_index].m_depth.Get(),
        RHICommandList::TransitionDesc{
            .src_access = RHIResourceAccess::Undefined,
            .dst_access = (RHIResourceAccess)(RHIResourceAccess::DepthStencilRead | RHIResourceAccess::DepthStencilWrite),
            .src_stage = RHIPipelineStage::TopOfPipe,
            .dst_stage = (RHIPipelineStage)(RHIPipelineStage::DepthStencilRead | RHIPipelineStage::DepthStencilWrite),
            .src_img_layout = RHITextureLayout::Undefined,
            .dst_img_layout = RHITextureLayout::DepthStencil,
            .src_img_range = {
                .layer = {.access = RHITextureAccessFlag::Depth }
            }
        }
    );
    cmd->EndTransition();
    cmd->BeginGraphics(RHICommandList::GraphicsDesc{
        .color_attachments = {{
            {
                .image_view = m_swaps[image_index].m_swapchain_imageview.Get(),
                .image_layout = RHITextureLayout::RenderTarget,
                .clear_color = RHIClearColor{ 0.0f, 0.0f, 0.0f, 1.0f },
            }
        }},
        .depth_attachment = {
            .image_view = m_swaps[image_index].m_depth_view.Get(),
            .image_layout = RHITextureLayout::DepthStencil,
            .clear_depth_stencil = RHIClearDepthStencil{ 1.0f, 0 },
        },
        .width = 1920,
        .height = 1080
    });
    cmd->SetPipeline(RHICommandList::PipelineDesc{
        .pipeline = m_pso.Get(),
        .type = RHIDevicePipelineType::Graphics
        });
    cmd->BindVertexBuffer(0, { { m_vertex_buffer.Get() } }, { {0} });
    cmd->BindIndexBuffer(m_index_buffer.Get(), 0, RHIResourceFormat::R32_UINT);
    cmd->SetViewport(0.0f, 0.0f, 1920.0f, 1080.0f);
    cmd->SetScissor(0, 0, 1920, 1080);
    cmd->BindDescriptorSet(
        RHIDevicePipelineType::Graphics,
        m_pso.Get(),
        { { m_desc_set[image_index].Get() } }
    );
    cmd->DrawIndexed(m_num_indices);
    cmd->EndGraphics();
    cmd->BeginTransition();
    cmd->SetImageTransition(
        m_swapchain->GetImages()[image_index],
        RHICommandList::TransitionDesc{
            .src_access = RHIResourceAccess::RenderTargetWrite,
            .dst_access = RHIResourceAccess::Undefined,
            .src_stage = RHIPipelineStage::RenderTargetOutput,
            .dst_stage = RHIPipelineStage::BottomOfPipe,
            .src_img_layout = RHITextureLayout::RenderTarget,
            .dst_img_layout = RHITextureLayout::Present
        }
    );
    cmd->EndTransition();
    cmd->End();
}
void Renderer::Draw() {
    // Update MVP
    static auto startTime = std::chrono::high_resolution_clock::now();
    auto currentTime = std::chrono::high_resolution_clock::now();
    float time = std::chrono::duration<float>(currentTime - startTime).count();
    auto& ubo = m_swaps[m_current_swap].m_uniform_buffer->MapSpan<uniform_buffer>()[0];
    ubo.model = glm::rotate(glm::mat4(1.0f), glm::radians(90.0f), glm::vec3(1.0f, 0.0f, 0.0f)) * glm::rotate(glm::mat4(1.0f), time * glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    ubo.view = glm::lookAt(glm::vec3(2.0f, 2.0f, 2.0f), glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    ubo.proj = glm::infinitePerspective(
        glm::radians(45.0f),
        m_swapchain->GetAspectRatio(),
        0.1f
    );
    ubo.proj[1][1] *= -1;

    m_device->WaitForFences(m_swaps[m_current_swap].m_fence_draw, true, -1);
    uint32_t image_index = m_swapchain->GetNextImage(-1, m_swaps[m_current_swap].m_sync_present, {});
    m_device->ResetFences(m_swaps[m_current_swap].m_fence_draw);
    m_swaps[m_current_swap].m_cmd->Reset();
    Record(image_index, m_swaps[m_current_swap].m_cmd.Get());
    m_queue->Submit(RHIDeviceQueue::SubmitDesc{
        .stages = RHIPipelineStage::RenderTargetOutput,
        .waits = m_swaps[m_current_swap].m_sync_present,
        .signals = m_swaps[m_current_swap].m_sync_draw,
        .cmd_lists = m_swaps[m_current_swap].m_cmd,
        .fence = m_swaps[m_current_swap].m_fence_draw
        });
    m_device->WaitForFences({ m_swaps[m_current_swap].m_fence_draw }, true, -1);
    m_queue->Present(RHIDeviceQueue::PresentDesc{
        .image_index = image_index,
        .swapchain = m_swapchain,
        .waits = m_swaps[m_current_swap].m_sync_draw
        });
    m_current_swap = (m_current_swap + 1) % m_swaps.size();
}
Renderer::~Renderer() {
    // Ensures that all commands are finished before destruction
    // XXX: GUARDS? But we don't own them!
    m_queue->WaitIdle();
}
