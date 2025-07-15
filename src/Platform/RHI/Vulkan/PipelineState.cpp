#include <Platform/RHI/Vulkan/Resource.hpp>
#include <Platform/RHI/Vulkan/Device.hpp>
#include <Platform/RHI/Vulkan/Shader.hpp>
#include <Platform/RHI/Vulkan/PipelineState.hpp>
#include <Core/Allocator/StackAllocator.hpp>
using namespace Foundation::Platform::RHI;
VulkanPipelineState::VulkanPipelineState(const VulkanDevice& device, PipelineStateDesc const& desc)
    : RHIPipelineState(device, desc), m_device(device) {
    Core::StackArena<> arena; Core::StackAllocatorSingleThreaded alloc(arena);
    vk::PipelineVertexInputStateCreateInfo vtx{}; // TODO: Binding
    vk::PipelineInputAssemblyStateCreateInfo ia{
        .topology = GetVulkanPrimitiveTopologyFromDesc(desc.topology),
        .primitiveRestartEnable = VK_FALSE
    };
    // We'll use dynamic viewport and scissor later
    vk::PipelineViewportStateCreateInfo viewport{ .viewportCount = 1, .scissorCount = 1 };
    vk::PipelineRasterizationStateCreateInfo rasterizer{
        .depthClampEnable = vk::False,
        .rasterizerDiscardEnable = vk::False,
        .polygonMode = GetVulkanPolygonModeFromDesc(desc.rasterizer.fill_mode),
        .cullMode = GetVulkanCullModeFromDesc(desc.rasterizer.cull_mode),
        .frontFace = GetVulkanFrontFaceFromDesc(desc.rasterizer.front_face),
        .depthBiasEnable = desc.rasterizer.enable_depth_bias,
        .depthBiasSlopeFactor = desc.rasterizer.depth_bias,
        .lineWidth = desc.rasterizer.line_fill_width
    };
    vk::PipelineMultisampleStateCreateInfo multisampling{
        .rasterizationSamples = GetVulkanSampleCountFromDesc(desc.multisample.sample_count),
        .sampleShadingEnable = desc.multisample.enabled,
    };
    std::vector<vk::PipelineColorBlendAttachmentState, Core::StlAllocator<vk::PipelineColorBlendAttachmentState>> blend_attachments(alloc.Ptr());
    std::vector<vk::Format, Core::StlAllocator<vk::Format>> color_attachment_formats(alloc.Ptr());
    for (const auto& attachment : desc.attachments) {
        color_attachment_formats.push_back(vkFormatFromRHIFormat(attachment.render_target.format));
        vk::PipelineColorBlendAttachmentState blend_attachment{
            .blendEnable = attachment.blending.enabled,
            .srcColorBlendFactor = GetVulkanBlendFactorFromDesc(attachment.blending.src_color_blend_factor),
            .dstColorBlendFactor = GetVulkanBlendFactorFromDesc(attachment.blending.dst_color_blend_factor),
            .colorBlendOp = GetVulkanBlendOpFromDesc(attachment.blending.color_blend_op),
            .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                              vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA
        };
        blend_attachments.push_back(blend_attachment);
    }
    vk::PipelineColorBlendStateCreateInfo color_blending{
        .logicOpEnable = VK_FALSE,
        .logicOp = vk::LogicOp::eCopy, // TODO
        .attachmentCount = static_cast<uint32_t>(blend_attachments.size()),
        .pAttachments = blend_attachments.data(),
    };
    vk::DynamicState dynamic_states[2] {vk::DynamicState::eViewport, vk::DynamicState::eScissor};
    vk::PipelineDynamicStateCreateInfo dynamic_state{
        .dynamicStateCount = 2, // Viewport and Scissor
        .pDynamicStates = dynamic_states
    };
    auto pipeline_layout = vk::raii::PipelineLayout(m_device.GetVkDevice(), vk::PipelineLayoutCreateInfo{
        .setLayoutCount = 0, // No descriptor sets for now
        .pushConstantRangeCount = 0 // No push constants for now
    }, m_device.GetVkAllocatorCallbacks());
    vk::PipelineRenderingCreateInfo rendering_create_info{
        .colorAttachmentCount = static_cast<uint32_t>(color_attachment_formats.size()),
        .pColorAttachmentFormats = color_attachment_formats.data()
    };
    std::vector<vk::PipelineShaderStageCreateInfo, Core::StlAllocator<vk::PipelineShaderStageCreateInfo>> shaderStages(alloc.Ptr());
    for (auto& shader : m_desc.shader_stages)
        shaderStages.push_back({
        .stage = GetVulkanShaderStageFromDesc(shader.desc.stage),
        .module = shader.shader_module.Get<VulkanShaderModule>()->GetVkShaderModule(),
        .pName = shader.desc.entry_point.c_str(),
        .pSpecializationInfo = nullptr // TODO: !! handle specialization info
    });
    vk::GraphicsPipelineCreateInfo pipelineInfo{ .pNext = &rendering_create_info,
        .stageCount = static_cast<uint32_t>(shaderStages.size()), .pStages = shaderStages.data(),
        .pVertexInputState = &vtx, .pInputAssemblyState = &ia,
        .pViewportState = &viewport, .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisampling, .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_state, .layout = pipeline_layout, .renderPass = nullptr };

    m_pipeline = vk::raii::Pipeline(m_device.GetVkDevice(), nullptr, pipelineInfo, m_device.GetVkAllocatorCallbacks());
}   
