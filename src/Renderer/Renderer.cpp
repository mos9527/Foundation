#include <fstream>
#include <filesystem>
#include <Renderer/Renderer.hpp>
#include <Core/Math.hpp>

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>

#include <array>
using namespace Foundation::Platform::RHI;
using namespace Foundation::Renderer;
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
struct vertex_input {
    glm::vec2 pos;
    glm::vec3 color;
    glm::vec2 texCoord;
};
struct uniform_buffer {
    // col-major
    glm::mat4 model;
    glm::mat4 view;
    glm::mat4 proj;
};
Renderer::Renderer(RHIApplicationObjectHandle<RHIDevice> device, Core::Allocator* allocator)
    : m_device(device), m_allocator(allocator), m_swapchain_imageviews(allocator),
    m_sync_present(allocator), m_sync_draw(allocator), m_fence_draw(allocator), m_cmd(allocator),
    m_desc_set(allocator), m_uniform_buffer(allocator) {
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
    for (auto const& image : m_swapchain->GetImages()) {
        m_sync_present.push_back(m_device->CreateSemaphore());
        m_sync_draw.push_back(m_device->CreateSemaphore());
        m_fence_draw.push_back(m_device->CreateFence(true /* signaled */));
        m_cmd.push_back(m_cmd_pool->CreateCommandList());
        m_swapchain_imageviews.push_back(
            image->CreateImageView(RHIImageViewDesc{
                .format = RHIResourceFormat::R8G8B8A8_UNORM,
                .range = {
                    .layer = {
                        .mip_level = 0,
                        .base_array_layer = 0,
                        .layer_count = 1
                    },
                    .mip_count = 1
                }
                })
        );
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
    RHIPipelineState::PipelineStateDesc::Attachment attachment{
        .blending = {.enabled = false },
        .render_target = {.format = RHIResourceFormat::R8G8B8A8_UNORM }
    };
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
                {{.stride = sizeof(vertex_input) }}
            },
            .attributes = {{
                {.location = 0, .binding = 0, .offset = offsetof(vertex_input, pos), .format = RHIResourceFormat::R32G32_SIGNED_FLOAT },
                {.location = 1, .binding = 0, .offset = offsetof(vertex_input, color), .format = RHIResourceFormat::R32G32B32_SIGNED_FLOAT },
                {.location = 2, .binding = 0, .offset = offsetof(vertex_input, texCoord), .format = RHIResourceFormat::R32G32_SIGNED_FLOAT }
            }}
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
        .attachments = { &attachment, 1 },
        .shader_stages = stages,
        .descriptor_set_layouts = { m_desc_layout },
    };
    m_pso = m_device->CreatePipelineState(pipeline);
    // Buffers    
    m_vertex_buffer = m_device->CreateBuffer(RHIBufferDesc{
        .resource = {
            .name = "Triangle Vertex Buffer",
            .host_access = RHIResourceHostAccess::ReadWrite,
        },
        .usage = (RHIBufferUsage)(RHIBufferUsage::VertexBuffer | RHIBufferUsage::TransferDestination),
        .size = 1 << 20 // 1 MiB
        });
    {
        // Demo: Staging buffer upload
        auto staging = m_device->CreateBuffer(RHIBufferDesc{
                .resource = {
                .host_access = RHIResourceHostAccess::ReadWrite,
                .coherent = true,
            },
            .usage = RHIBufferUsage::TransferSource,
            .size = 1 << 20 // 1 MiB
            });
        auto cmd = m_cmd_pool->CreateCommandList();
        {
            auto span = staging->MapSpan<vertex_input>(4);
            span[0] = { {-0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}, {1.0f, 0.0f} },
                span[1] = { {0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f} },
                span[2] = { {0.5f, 0.5f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f} },
                span[3] = { {-0.5f, 0.5f}, {1.0f, 1.0f, 1.0f}, {1.0f, 1.0f} };
            cmd->Begin();
            cmd->CopyBuffer(
                staging.Get(),
                m_vertex_buffer.Get(),
                { {RHICommandList::CopyBufferRegion{
                    .src_offset = 0,
                    .dst_offset = 0,
                    .size = span.size_bytes()
                }} }
            );
            cmd->End();
            m_queue->Submit(RHIDeviceQueue::SubmitDesc{
                .cmd_lists = cmd,
                });
            m_queue->WaitIdle();
        }
        {
            std::array<uint16_t, 6> index{ 0, 1, 2, 2, 3, 0 };
            auto span = staging->MapSpan<decltype(index)>();
            span[0] = index;            
            m_index_buffer = m_device->CreateBuffer(RHIBufferDesc{
                .resource = {
                    .name = "Triangle Index Buffer",
                    .host_access = RHIResourceHostAccess::ReadWrite,
                },
                .usage = (RHIBufferUsage)(RHIBufferUsage::IndexBuffer | RHIBufferUsage::TransferDestination),
                .size = span.size_bytes()
            });
            cmd->Begin();
            cmd->CopyBuffer(
                staging.Get(),
                m_index_buffer.Get(),
                { { {.size = index.size() * sizeof(uint16_t)} } }
            );
            cmd->End();
            m_queue->Submit(RHIDeviceQueue::SubmitDesc{
                .cmd_lists = cmd,
                });
            m_queue->WaitIdle();
        }
    }
    // Images
    {
        int w, h, ch;
        stbi_uc* pixels = stbi_load(".derived/texture.jpg", &w, &h, &ch, STBI_rgb_alpha);
        ch = 4; // XXX: ch is the reported channel count - stb WILL load the image as RGBARGBA... row major
        CHECK(pixels && "Failed to load texture image");
        m_tex = m_device->CreateImage(RHIImageDesc{
            .resource = {
                .name = "Statue Texture",
                .host_access = RHIResourceHostAccess::Invisible,
            },
            .usage = (RHIImageUsage)(RHIImageUsage::SampledImage | RHIImageUsage::TransferDestination),
            .extent = { w, h, 1 },
            .format = RHIResourceFormat::R8G8B8A8_UNORM,
            .initial_layout = RHIImageLayout::Undefined,
            });
        m_tex_view = m_tex->CreateImageView(RHIImageViewDesc{
            .format = RHIResourceFormat::R8G8B8A8_UNORM,
            });
        // Staging again!
        auto staging = m_device->CreateBuffer(RHIBufferDesc{
            .resource = {
                .host_access = RHIResourceHostAccess::ReadWrite,
                .coherent = true,
            },
            .usage = RHIBufferUsage::TransferSource,
            .size = static_cast<size_t>(w * h * ch)
            });
        memcpy(staging->Map(), pixels, static_cast<size_t>(w * h * ch));
        stbi_image_free(pixels);
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
                .src_img_layout = RHIImageLayout::Undefined,
                .dst_img_layout = RHIImageLayout::TransferDst
            }
        );
        cmd->EndTransition();
        cmd->CopyBufferToImage(
            staging.Get(),
            m_tex.Get(),
            RHIImageLayout::TransferDst,
            { {RHICommandList::CopyImageRegion{
                .extent = { static_cast<uint32_t>(w), static_cast<uint32_t>(h), 1 }
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
                .src_img_layout = RHIImageLayout::TransferDst,
                .dst_img_layout = RHIImageLayout::ShaderReadOnly
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
    for (auto& set : m_desc_set) {
        set = m_desc_pool->CreateDescriptorSet(m_desc_layout);
        auto& buffer = m_uniform_buffer.emplace_back(m_device->CreateBuffer(RHIBufferDesc{
            .resource = {
                .name = "Triangle Uniform Buffer",
                .host_access = RHIResourceHostAccess::ReadWrite,
                .coherent = true
            },
            .usage = (RHIBufferUsage)(RHIBufferUsage::UniformBuffer | RHIBufferUsage::TransferDestination),
            .size = sizeof(uniform_buffer)
            }));
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
                {.image_view = m_tex_view.Get(), .layout = RHIImageLayout::ShaderReadOnly }
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
            .src_img_layout = RHIImageLayout::Undefined,
            .dst_img_layout = RHIImageLayout::RenderTarget
        }
    );
    cmd->EndTransition();
    auto render_target = RHICommandList::GraphicsDesc::Attachment{
                .image_view = m_swapchain_imageviews[image_index].Get(),
                .image_layout = RHIImageLayout::RenderTarget,
                .clear_color = RHIClearColor{ 0.0f, 0.0f, 0.0f, 1.0f },
    };
    cmd->BeginGraphics(RHICommandList::GraphicsDesc{
        .attachments = { &render_target , 1 },
        .width = 1920,
        .height = 1080
        });
    cmd->SetPipeline(RHICommandList::PipelineDesc{
        .pipeline = m_pso.Get(),
        .type = RHIDevicePipelineType::Graphics
        });
    cmd->BindVertexBuffer(0, { { m_vertex_buffer.Get() } }, { {0} });
    cmd->BindIndexBuffer(m_index_buffer.Get(), 0, RHIResourceFormat::R16_UINT);
    cmd->SetViewport(0.0f, 0.0f, 1920.0f, 1080.0f);
    cmd->SetScissor(0, 0, 1920, 1080);
    cmd->BindDescriptorSet(
        RHIDevicePipelineType::Graphics,
        m_pso.Get(),
        { { m_desc_set[image_index].Get() } }
    );
    cmd->DrawIndexed(6);
    cmd->EndGraphics();
    cmd->BeginTransition();
    cmd->SetImageTransition(
        m_swapchain->GetImages()[image_index],
        RHICommandList::TransitionDesc{
            .src_access = RHIResourceAccess::RenderTargetWrite,
            .dst_access = RHIResourceAccess::Undefined,
            .src_stage = RHIPipelineStage::RenderTargetOutput,
            .dst_stage = RHIPipelineStage::BottomOfPipe,
            .src_img_layout = RHIImageLayout::RenderTarget,
            .dst_img_layout = RHIImageLayout::Present
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
    auto& ubo = m_uniform_buffer[m_current_img]->MapSpan<uniform_buffer>()[0];
    ubo.model = glm::rotate(glm::mat4(1.0f), time * glm::radians(90.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    ubo.view = glm::lookAt(glm::vec3(2.0f, 2.0f, 2.0f), glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    ubo.proj = glm::infinitePerspective(
        glm::radians(45.0f),
        m_swapchain->GetAspectRatio(),
        0.1f
    );
    ubo.proj[1][1] *= -1;

    m_device->WaitForFences(m_fence_draw[m_current_img], true, -1);
    uint32_t image_index = m_swapchain->GetNextImage(-1, m_sync_present[m_current_img], {});
    m_device->ResetFences(m_fence_draw[m_current_img]);
    m_cmd[m_current_img]->Reset();
    Record(image_index, m_cmd[m_current_img].Get());
    m_queue->Submit(RHIDeviceQueue::SubmitDesc{
        .stages = RHIPipelineStage::RenderTargetOutput,
        .waits = m_sync_present[m_current_img],
        .signals = m_sync_draw[m_current_img],
        .cmd_lists = m_cmd[m_current_img],
        .fence = m_fence_draw[m_current_img]
        });
    m_device->WaitForFences({ m_fence_draw[m_current_img] }, true, -1);
    m_queue->Present(RHIDeviceQueue::PresentDesc{
        .image_index = image_index,
        .swapchain = m_swapchain,
        .waits = m_sync_draw[m_current_img]
        });
    m_current_img = (m_current_img + 1) % (m_cmd.size());
}
Renderer::~Renderer() {
    // Ensures that all commands are finished before destruction
    // XXX: GUARDS? But we don't own them!
    m_queue->WaitIdle();
}
