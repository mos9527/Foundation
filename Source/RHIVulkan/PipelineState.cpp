using namespace Foundation::RHI;
void VulkanPipelineState::InitializePipelineLayout() {
    StackArena<> arena; AllocatorStack alloc(arena);
    Vector<vk::DescriptorSetLayout> p_set_layouts(mDesc.descriptorSetLayouts.size(), alloc.Ptr());
    for (size_t i = 0; i < mDesc.descriptorSetLayouts.size(); ++i)
        p_set_layouts[i] = static_cast<VulkanDeviceDescriptorSetLayout*>(mDesc.descriptorSetLayouts[i])->GetVkLayout();
    Vector<vk::PushConstantRange> push_constants(mDesc.pushConstants.size(), alloc.Ptr());
    for (size_t i = 0; i < mDesc.pushConstants.size(); i++) {
        const auto& [stage, offset, size] = mDesc.pushConstants[i];
        CHECK_MSG(size <= 128, "Push constant size exceeds min spec limit");
        push_constants[i]
            .setStageFlags(vkShaderStageFlagsFromRHIShaderStage(stage))
            .setOffset(offset)
            .setSize(size);
    }
    mPipelineLayout = vk::raii::PipelineLayout(mDevice.GetVkDevice(),
        vk::PipelineLayoutCreateInfo{
            .setLayoutCount = static_cast<uint32_t>(p_set_layouts.size()),
            .pSetLayouts = p_set_layouts.data(),
            .pushConstantRangeCount = static_cast<uint32_t>(push_constants.size()),
            .pPushConstantRanges = push_constants.data()
    }, mDevice.GetVkAllocatorCallbacks());
}
void VulkanPipelineState::InitializeGraphics() {
    StackArena<> arena; AllocatorStack alloc(arena);
    Vector<vk::VertexInputBindingDescription> vtx_bindings(alloc.Ptr());
    for (size_t i = 0; i < mDesc.vertexInput.bindings.size(); ++i) {
        const auto& [stride, per_instance] = mDesc.vertexInput.bindings[i];
        vtx_bindings.emplace_back(vk::VertexInputBindingDescription{
            .binding = static_cast<uint32_t>(i),
            .stride = stride,
            .inputRate = per_instance ? vk::VertexInputRate::eInstance : vk::VertexInputRate::eVertex
        });
    }
    Vector<vk::VertexInputAttributeDescription> vtx_attrs(alloc.Ptr());
    Set<uint32_t> bindings_used(alloc.Ptr());
    for (const auto& [location, offset, format, binding] : mDesc.vertexInput.attributes) {
        vtx_attrs.emplace_back(vk::VertexInputAttributeDescription{
            .location = location,
            .binding = binding,
            .format = vkFormatFromRHIFormat(format),
            .offset = offset
        });
        bindings_used.insert(binding);
    }
    vk::PipelineVertexInputStateCreateInfo vtx{
        .vertexBindingDescriptionCount = static_cast<uint32_t>(vtx_bindings.size()),
        .pVertexBindingDescriptions = vtx_bindings.data(),
        .vertexAttributeDescriptionCount = static_cast<uint32_t>(vtx_attrs.size()),
        .pVertexAttributeDescriptions = vtx_attrs.data(),
    };
    vk::PipelineInputAssemblyStateCreateInfo ia{
        .topology = GetVulkanPrimitiveTopologyFromDesc(mDesc.topology),
        .primitiveRestartEnable = VK_FALSE
    };
    vk::CompareOp depth_compare_op{ vk::CompareOp::eLess };
    switch (mDesc.depthStencil.depthCompareOp)
    {
    case PipelineStateDesc::DepthStencil::Never:
        depth_compare_op = vk::CompareOp::eNever; break;
    case PipelineStateDesc::DepthStencil::Less:
        depth_compare_op = vk::CompareOp::eLess; break;
    case PipelineStateDesc::DepthStencil::Equal:
        depth_compare_op = vk::CompareOp::eEqual; break;
    case PipelineStateDesc::DepthStencil::LessEqual:
        depth_compare_op = vk::CompareOp::eLessOrEqual; break;
    case PipelineStateDesc::DepthStencil::Greater:
        depth_compare_op = vk::CompareOp::eGreater; break;
    case PipelineStateDesc::DepthStencil::NotEqual:
        depth_compare_op = vk::CompareOp::eNotEqual; break;
    case PipelineStateDesc::DepthStencil::GreaterEqual:
        depth_compare_op = vk::CompareOp::eGreaterOrEqual; break;
    case PipelineStateDesc::DepthStencil::Always:
        depth_compare_op = vk::CompareOp::eAlways; break;
    default:
        break;
    }
    vk::PipelineDepthStencilStateCreateInfo depth_stencil{
        .depthTestEnable = mDesc.depthStencil.depthTest,
        .depthWriteEnable = mDesc.depthStencil.depthWrite,
        .depthCompareOp = depth_compare_op,
    };
    // We'll use dynamic viewport and scissor later
    vk::PipelineViewportStateCreateInfo viewport{ .viewportCount = 1, .scissorCount = 1 };
    vk::PipelineRasterizationStateCreateInfo rasterizer{
        .depthClampEnable = vk::False,
        .rasterizerDiscardEnable = vk::False,
        .polygonMode = GetVulkanPolygonModeFromDesc(mDesc.rasterizer.fillMode),
        .cullMode = GetVulkanCullModeFromDesc(mDesc.rasterizer.cullMode),
        .frontFace = GetVulkanFrontFaceFromDesc(mDesc.rasterizer.frontFace),
        .depthBiasEnable = mDesc.rasterizer.enableDepthBias,
        .depthBiasSlopeFactor = mDesc.rasterizer.depthBias,
        .lineWidth = mDesc.rasterizer.lineFillWidth
    };
    vk::PipelineMultisampleStateCreateInfo multisampling{
        .rasterizationSamples = vkSampleCountFlagFromRHIMultisampleCount(mDesc.multisample.sampleCount),
        .sampleShadingEnable = mDesc.multisample.enabled,
    };
    Vector<vk::PipelineColorBlendAttachmentState> blend_attachments(alloc.Ptr());
    Vector<vk::Format> color_attachment_formats(alloc.Ptr());
    for (const auto& attachment : mDesc.attachments) {
        color_attachment_formats.push_back(vkFormatFromRHIFormat(attachment.renderTarget.format));
        vk::PipelineColorBlendAttachmentState blend_attachment{
            .blendEnable = attachment.blending.enabled,
            .srcColorBlendFactor = GetVulkanBlendFactorFromDesc(attachment.blending.srcColorBlendFactor),
            .dstColorBlendFactor = GetVulkanBlendFactorFromDesc(attachment.blending.dstColorBlendFactor),
            .colorBlendOp = GetVulkanBlendOpFromDesc(attachment.blending.colorBlendOp),
            .srcAlphaBlendFactor = GetVulkanBlendFactorFromDesc(attachment.blending.srcAlphaBlendFactor),
            .dstAlphaBlendFactor = GetVulkanBlendFactorFromDesc(attachment.blending.dstAlphaBlendFactor),
            .alphaBlendOp = GetVulkanBlendOpFromDesc(attachment.blending.alphaBlendOp),
            .colorWriteMask = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                              vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA
        };
        blend_attachments.push_back(blend_attachment);
    }
    vk::PipelineColorBlendStateCreateInfo color_blending{
        .logicOpEnable = VK_FALSE,
        .logicOp = vk::LogicOp::eCopy, // TODO - logicOp unused for now
        .attachmentCount = static_cast<uint32_t>(blend_attachments.size()),
        .pAttachments = blend_attachments.data(),
        .blendConstants = Array<float,4>{1.0f, 1.0f, 1.0f, 1.0f}
    };
    vk::DynamicState dynamic_states[2]{ vk::DynamicState::eViewport, vk::DynamicState::eScissor };
    vk::PipelineDynamicStateCreateInfo dynamic_state{
        .dynamicStateCount = 2, // Viewport and Scissor
        .pDynamicStates = dynamic_states
    };
    vk::PipelineRenderingCreateInfo rendering_create_info{
        .colorAttachmentCount = static_cast<uint32_t>(color_attachment_formats.size()),
        .pColorAttachmentFormats = color_attachment_formats.data(),
        .depthAttachmentFormat = vkFormatFromRHIFormat(mDesc.depthStencil.depthFormat),
        .stencilAttachmentFormat = vkFormatFromRHIFormat(mDesc.depthStencil.stencilFormat),
    };
    Vector<vk::PipelineShaderStageCreateInfo> shaderStages(alloc.Ptr());
    for (auto& shader : mDesc.shaderStages)
        shaderStages.push_back({
            .stage = vkFlagsToBits(vkShaderStageFlagsFromRHIShaderStage(shader.desc.stage)),
            .module = static_cast<VulkanShaderModule*>(shader.shaderModule)->GetVkShaderModule(),
            .pName = shader.desc.entryPoint,
            .pSpecializationInfo = nullptr // TODO: Handle specialization info
        });
    CHECK_MSG(!shaderStages.empty(), "At least one shader stage must be specified in a graphics pipeline");
    vk::GraphicsPipelineCreateInfo pipelineInfo{ .pNext = &rendering_create_info,
        .stageCount = static_cast<uint32_t>(shaderStages.size()), .pStages = shaderStages.data(),
        .pVertexInputState = &vtx, .pInputAssemblyState = &ia,
        .pViewportState = &viewport, .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisampling, .pDepthStencilState = &depth_stencil, .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_state, .layout = mPipelineLayout, .renderPass = nullptr };

    mPipeline = vk::raii::Pipeline(mDevice.GetVkDevice(), nullptr, pipelineInfo, mDevice.GetVkAllocatorCallbacks());
}
void VulkanPipelineState::InitializeCompute() {
    CHECK_MSG(mDesc.shaderStages.size() == 1, "Compute pipeline must have exactly 1 shader stage.");
    auto const& shader_stage = mDesc.shaderStages[0];
    CHECK_MSG(
        shader_stage.desc.stage.is_bitmask() &&
        shader_stage.desc.stage == RHIShaderStageBits::Compute,
        "Compute stage must contain only Compute shaders"
    );
    vk::PipelineShaderStageCreateInfo stage_info{
        .stage = vk::ShaderStageFlagBits::eCompute,
        .module =static_cast<VulkanShaderModule*>(shader_stage.shaderModule)->GetVkShaderModule(),
        .pName = shader_stage.desc.entryPoint,
        .pSpecializationInfo = nullptr // TODO: Handle specialization info
    };
    vk::ComputePipelineCreateInfo pipelineInfo{
        .stage = stage_info,
        .layout = mPipelineLayout
    };
    mPipeline = vk::raii::Pipeline(mDevice.GetVkDevice(), nullptr, pipelineInfo, mDevice.GetVkAllocatorCallbacks());
}
VulkanPipelineState::VulkanPipelineState(const VulkanDevice& device, PipelineStateDesc const& desc)
    : RHIPipelineState(device, desc), mDevice(device) {
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
    VkPipeline handle = *mPipeline;
    mDevice.GetVkDevice().setDebugUtilsObjectNameEXT({
        .objectType = vk::ObjectType::ePipeline,
        .objectHandle = reinterpret_cast<uint64_t>(handle),
        .pObjectName = name
    });
}
