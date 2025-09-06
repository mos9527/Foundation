#include "Resource.hpp"
#include "Device.hpp"
#include "Shader.hpp"
#include "PipelineState.hpp"
#include <Core/Allocator/StackAllocator.hpp>
using namespace Foundation::RHI;
void VulkanPipelineState::InitializePipelineLayout() {
    Core::StackArena<> arena; Core::StackAllocatorSingleThreaded alloc(arena);
    Core::StlVector<vk::DescriptorSetLayout> p_set_layouts(m_desc.descriptor_set_layouts.size(), alloc.Ptr());
    for (size_t i = 0; i < m_desc.descriptor_set_layouts.size(); ++i)
        p_set_layouts[i] = m_desc.descriptor_set_layouts[i].Get<VulkanDeviceDescriptorSetLayout>()->GetVkLayout();
    Core::StlVector<vk::PushConstantRange> push_constants(m_desc.push_constants.size(), alloc.Ptr());
    for (size_t i = 0; i < m_desc.push_constants.size(); i++) {
        auto& pdesc = m_desc.push_constants[i];
        push_constants[i]
            .setStageFlags(vkShaderStageFlagsFromRHIShaderStage(pdesc.stage))
            .setOffset(pdesc.offset)
            .setSize(pdesc.size);
    }
    m_pipeline_layout = vk::raii::PipelineLayout(m_device.GetVkDevice(),
        vk::PipelineLayoutCreateInfo{
            .setLayoutCount = static_cast<uint32_t>(p_set_layouts.size()),
            .pSetLayouts = p_set_layouts.data(),
            .pushConstantRangeCount = static_cast<uint32_t>(push_constants.size()),
            .pPushConstantRanges = push_constants.data()
    }, m_device.GetVkAllocatorCallbacks());
}
void VulkanPipelineState::InitializeGraphics() {
    Core::StackArena<> arena; Core::StackAllocatorSingleThreaded alloc(arena);
    Core::StlVector<vk::VertexInputBindingDescription> vtx_bindings(alloc.Ptr());
    for (size_t i = 0; i < m_desc.vertex_input.bindings.size(); ++i) {
        const auto& binding = m_desc.vertex_input.bindings[i];
        vtx_bindings.emplace_back(vk::VertexInputBindingDescription{
            .binding = static_cast<uint32_t>(i),
            .stride = binding.stride,
            .inputRate = binding.per_instance ? vk::VertexInputRate::eInstance : vk::VertexInputRate::eVertex
            });
    }
    Core::StlVector<vk::VertexInputAttributeDescription> vtx_attrs(alloc.Ptr());
    for (const auto& attr : m_desc.vertex_input.attributes) {
        vtx_attrs.emplace_back(vk::VertexInputAttributeDescription{
            .location = attr.location,
            .binding = attr.binding,
            .format = vkFormatFromRHIFormat(attr.format),
            .offset = attr.offset
            });
    }
    vk::PipelineVertexInputStateCreateInfo vtx{
        .vertexBindingDescriptionCount = static_cast<uint32_t>(vtx_bindings.size()),
        .pVertexBindingDescriptions = vtx_bindings.data(),
        .vertexAttributeDescriptionCount = static_cast<uint32_t>(vtx_attrs.size()),
        .pVertexAttributeDescriptions = vtx_attrs.data(),
    };
    vk::PipelineInputAssemblyStateCreateInfo ia{
        .topology = GetVulkanPrimitiveTopologyFromDesc(m_desc.topology),
        .primitiveRestartEnable = VK_FALSE
    };
    vk::CompareOp depth_compare_op{ vk::CompareOp::eLess };
    switch (m_desc.depth_stencil.depth_compare_op)
    {
    case PipelineStateDesc::DepthStencil::NEVER:
        depth_compare_op = vk::CompareOp::eNever; break;
    case PipelineStateDesc::DepthStencil::LESS:
        depth_compare_op = vk::CompareOp::eLess; break;
    case PipelineStateDesc::DepthStencil::EQUAL:
        depth_compare_op = vk::CompareOp::eEqual; break;
    case PipelineStateDesc::DepthStencil::LESS_EQUAL:
        depth_compare_op = vk::CompareOp::eLessOrEqual; break;
    case PipelineStateDesc::DepthStencil::GREATER:
        depth_compare_op = vk::CompareOp::eGreater; break;
    case PipelineStateDesc::DepthStencil::NOT_EQUAL:
        depth_compare_op = vk::CompareOp::eNotEqual; break;
    case PipelineStateDesc::DepthStencil::GREATER_EQUAL:
        depth_compare_op = vk::CompareOp::eGreaterOrEqual; break;
    case PipelineStateDesc::DepthStencil::ALWAYS:
        depth_compare_op = vk::CompareOp::eAlways; break;
    default:
        break;
    }
    vk::PipelineDepthStencilStateCreateInfo depth_stencil{
        .depthTestEnable = m_desc.depth_stencil.depth_test,
        .depthWriteEnable = m_desc.depth_stencil.depth_write,
        .depthCompareOp = depth_compare_op,
    };
    // We'll use dynamic viewport and scissor later
    vk::PipelineViewportStateCreateInfo viewport{ .viewportCount = 1, .scissorCount = 1 };
    vk::PipelineRasterizationStateCreateInfo rasterizer{
        .depthClampEnable = vk::False,
        .rasterizerDiscardEnable = vk::False,
        .polygonMode = GetVulkanPolygonModeFromDesc(m_desc.rasterizer.fill_mode),
        .cullMode = GetVulkanCullModeFromDesc(m_desc.rasterizer.cull_mode),
        .frontFace = GetVulkanFrontFaceFromDesc(m_desc.rasterizer.front_face),
        .depthBiasEnable = m_desc.rasterizer.enable_depth_bias,
        .depthBiasSlopeFactor = m_desc.rasterizer.depth_bias,
        .lineWidth = m_desc.rasterizer.line_fill_width
    };
    vk::PipelineMultisampleStateCreateInfo multisampling{
        .rasterizationSamples = vkSampleCountFlagFromRHIMultisampleCount(m_desc.multisample.sample_count),
        .sampleShadingEnable = m_desc.multisample.enabled,
    };
    Core::StlVector<vk::PipelineColorBlendAttachmentState> blend_attachments(alloc.Ptr());
    Core::StlVector<vk::Format> color_attachment_formats(alloc.Ptr());
    for (const auto& attachment : m_desc.attachments) {
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
    vk::DynamicState dynamic_states[2]{ vk::DynamicState::eViewport, vk::DynamicState::eScissor };
    vk::PipelineDynamicStateCreateInfo dynamic_state{
        .dynamicStateCount = 2, // Viewport and Scissor
        .pDynamicStates = dynamic_states
    };
    vk::PipelineRenderingCreateInfo rendering_create_info{
        .colorAttachmentCount = static_cast<uint32_t>(color_attachment_formats.size()),
        .pColorAttachmentFormats = color_attachment_formats.data(),
        .depthAttachmentFormat = vkFormatFromRHIFormat(m_desc.depth_stencil.depth_format),
        .stencilAttachmentFormat = vkFormatFromRHIFormat(m_desc.depth_stencil.stencil_format),
    };
    Core::StlVector<vk::PipelineShaderStageCreateInfo> shaderStages(alloc.Ptr());
    for (auto& shader : m_desc.shader_stages)
        shaderStages.push_back({
            .stage = vkFlagsToBits(vkShaderStageFlagsFromRHIShaderStage(shader.desc.stage)),
            .module = shader.shader_module.Get<VulkanShaderModule>()->GetVkShaderModule(),
            .pName = shader.desc.entry_point,
            .pSpecializationInfo = nullptr // TODO: Handle specialization info
        });
    CHECK_MSG(shaderStages.size() > 0, "At least one shader stage must be specified in a graphics pipeline");
    vk::GraphicsPipelineCreateInfo pipelineInfo{ .pNext = &rendering_create_info,
        .stageCount = static_cast<uint32_t>(shaderStages.size()), .pStages = shaderStages.data(),
        .pVertexInputState = &vtx, .pInputAssemblyState = &ia,
        .pViewportState = &viewport, .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisampling, .pDepthStencilState = &depth_stencil, .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_state, .layout = m_pipeline_layout, .renderPass = nullptr };

    m_pipeline = vk::raii::Pipeline(m_device.GetVkDevice(), nullptr, pipelineInfo, m_device.GetVkAllocatorCallbacks());
}
void VulkanPipelineState::InitializeCompute() {
    Core::StackArena<> arena; Core::StackAllocatorSingleThreaded alloc(arena);
    CHECK_MSG(m_desc.shader_stages.size() == 1, "Compute pipeline must have exactly 1 shader stage.");
    auto const& shader_stage = m_desc.shader_stages[0];
    CHECK_MSG(
        shader_stage.desc.stage.is_bitmask() &&
        shader_stage.desc.stage == RHIShaderStageBits::Compute,
        "Compute stage must contain only Compute shaders"
    );
    vk::PipelineShaderStageCreateInfo stage_info{
        .stage = vk::ShaderStageFlagBits::eCompute,
        .module = shader_stage.shader_module.Get<VulkanShaderModule>()->GetVkShaderModule(),
        .pName = shader_stage.desc.entry_point,
        .pSpecializationInfo = nullptr // TODO: Handle specialization info
    };
    vk::ComputePipelineCreateInfo pipelineInfo{
        .stage = stage_info,
        .layout = m_pipeline_layout
    };
    m_pipeline = vk::raii::Pipeline(m_device.GetVkDevice(), nullptr, pipelineInfo, m_device.GetVkAllocatorCallbacks());
}
VulkanPipelineState::VulkanPipelineState(const VulkanDevice& device, PipelineStateDesc const& desc)
    : RHIPipelineState(device, desc), m_device(device) {
    InitializePipelineLayout();
    switch (desc.type)
    {
    case RHIDevicePipelineType::Graphics:
        InitializeGraphics();
        break;
    case RHIDevicePipelineType::Compute:
        InitializeCompute();
        break;    
    }
}

void VulkanPipelineState::DebugSetObjectName(const char* name) {
    VkPipeline handle = *m_pipeline;
    m_device.GetVkDevice().setDebugUtilsObjectNameEXT({
        .objectType = vk::ObjectType::ePipeline,
        .objectHandle = (uint64_t)(handle),
        .pObjectName = name
    });
}
