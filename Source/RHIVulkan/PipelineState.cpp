using namespace Foundation::RHI;

VulkanPipelineStateCache::VulkanPipelineStateCache(const VulkanDevice& device, PipelineStateCacheDesc const& desc) :
    RHIPipelineStateCache(device, desc), mDevice(device)
{
    mCache =
        vk::raii::PipelineCache(device.GetVkDevice(), vk::PipelineCacheCreateInfo{
                                    .initialDataSize = desc.initialData.size_bytes(),
                                    .pInitialData = desc.initialData.data()
                                }, nullptr);
}

size_t VulkanPipelineStateCache::GetCachedData(void* dstBuffer) const
{
    size_t size = 0;
    CHECK(vkGetPipelineCacheData(*mDevice.GetVkDevice(), *mCache, &size, dstBuffer) == VK_SUCCESS);
    return size;
}

void VulkanPipelineStateCache::DebugSetObjectName(const char* name)
{
    VkPipelineCache handle = *mCache;
    mDevice.GetVkDevice().setDebugUtilsObjectNameEXT({.objectType = vk::ObjectType::ePipelineCache,
                                                      .objectHandle = reinterpret_cast<uint64_t>(handle),
                                                      .pObjectName = name});
}

void VulkanPipelineState::InitializePipelineLayout()
{
    StackArena<> arena;
    AllocatorStack alloc(arena);
    Vector<vk::DescriptorSetLayout> p_set_layouts(mDesc.descriptorSetLayouts.size(), alloc.Ptr());
    for (size_t i = 0; i < mDesc.descriptorSetLayouts.size(); ++i)
    {
        CHECK_MSG(mDesc.descriptorSetLayouts[i], "Descriptor set layout MUST NOT be null");
        p_set_layouts[i] = static_cast<VulkanDeviceDescriptorSetLayout*>(mDesc.descriptorSetLayouts[i])->GetVkLayout();
    }
    Vector<vk::PushConstantRange> push_constants(mDesc.pushConstants.size(), alloc.Ptr());
    for (size_t i = 0; i < mDesc.pushConstants.size(); i++)
    {
        const auto& [stage, offset, size] = mDesc.pushConstants[i];
        CHECK_MSG(size <= 128, "Push constant size exceeds min spec limit");
        push_constants[i].setStageFlags(vkShaderStageFlagsFromRHIShaderStage(stage)).setOffset(offset).setSize(size);
    }
    mPipelineLayout = vk::raii::PipelineLayout(
        mDevice.GetVkDevice(),
        vk::PipelineLayoutCreateInfo{.setLayoutCount = static_cast<uint32_t>(p_set_layouts.size()),
                                     .pSetLayouts = p_set_layouts.data(),
                                     .pushConstantRangeCount = static_cast<uint32_t>(push_constants.size()),
                                     .pPushConstantRanges = push_constants.data()},
        nullptr);
}

void VulkanPipelineState::InitializeGraphics()
{
    StackArena<> arena;
    AllocatorStack alloc(arena);
    Vector<vk::VertexInputBindingDescription> vtx_bindings(alloc.Ptr());
    for (size_t i = 0; i < mDesc.vertexInput.bindings.size(); ++i)
    {
        const auto& [stride, per_instance] = mDesc.vertexInput.bindings[i];
        vtx_bindings.emplace_back(vk::VertexInputBindingDescription{
            .binding = static_cast<uint32_t>(i),
            .stride = stride,
            .inputRate = per_instance ? vk::VertexInputRate::eInstance : vk::VertexInputRate::eVertex});
    }
    Vector<vk::VertexInputAttributeDescription> vtx_attrs(alloc.Ptr());
    Set<uint32_t> bindings_used(alloc.Ptr());
    for (const auto& [location, offset, format, binding] : mDesc.vertexInput.attributes)
    {
        vtx_attrs.emplace_back(vk::VertexInputAttributeDescription{
            .location = location, .binding = binding, .format = vkFormatFromRHIFormat(format), .offset = offset});
        bindings_used.insert(binding);
    }
    vk::PipelineVertexInputStateCreateInfo vtx{
        .vertexBindingDescriptionCount = static_cast<uint32_t>(vtx_bindings.size()),
        .pVertexBindingDescriptions = vtx_bindings.data(),
        .vertexAttributeDescriptionCount = static_cast<uint32_t>(vtx_attrs.size()),
        .pVertexAttributeDescriptions = vtx_attrs.data(),
    };
    vk::PipelineInputAssemblyStateCreateInfo ia{.topology = GetVulkanPrimitiveTopologyFromDesc(mDesc.topology),
                                                .primitiveRestartEnable = VK_FALSE};
    vk::CompareOp depth_compare_op{vk::CompareOp::eLess};
    switch (mDesc.depthStencil.depthCompareOp)
    {
    case PipelineStateDesc::DepthStencil::Never:
        depth_compare_op = vk::CompareOp::eNever;
        break;
    case PipelineStateDesc::DepthStencil::Less:
        depth_compare_op = vk::CompareOp::eLess;
        break;
    case PipelineStateDesc::DepthStencil::Equal:
        depth_compare_op = vk::CompareOp::eEqual;
        break;
    case PipelineStateDesc::DepthStencil::LessEqual:
        depth_compare_op = vk::CompareOp::eLessOrEqual;
        break;
    case PipelineStateDesc::DepthStencil::Greater:
        depth_compare_op = vk::CompareOp::eGreater;
        break;
    case PipelineStateDesc::DepthStencil::NotEqual:
        depth_compare_op = vk::CompareOp::eNotEqual;
        break;
    case PipelineStateDesc::DepthStencil::GreaterEqual:
        depth_compare_op = vk::CompareOp::eGreaterOrEqual;
        break;
    case PipelineStateDesc::DepthStencil::Always:
        depth_compare_op = vk::CompareOp::eAlways;
        break;
    default:
        break;
    }
    vk::PipelineDepthStencilStateCreateInfo depth_stencil{
        .depthTestEnable = mDesc.depthStencil.depthTest,
        .depthWriteEnable = mDesc.depthStencil.depthWrite,
        .depthCompareOp = depth_compare_op,
    };
    // We'll use dynamic viewport and scissor later
    vk::PipelineViewportStateCreateInfo viewport{.viewportCount = 1, .scissorCount = 1};
    vk::PipelineRasterizationStateCreateInfo rasterizer{
        .depthClampEnable = vk::False,
        .rasterizerDiscardEnable = vk::False,
        .polygonMode = GetVulkanPolygonModeFromDesc(mDesc.rasterizer.fillMode),
        .cullMode = GetVulkanCullModeFromDesc(mDesc.rasterizer.cullMode),
        .frontFace = GetVulkanFrontFaceFromDesc(mDesc.rasterizer.frontFace),
        .depthBiasEnable = mDesc.rasterizer.enableDepthBias,
        .depthBiasSlopeFactor = mDesc.rasterizer.depthBias,
        .lineWidth = mDesc.rasterizer.lineFillWidth};
    vk::PipelineMultisampleStateCreateInfo multisampling{
        .rasterizationSamples = vkSampleCountFlagFromRHIMultisampleCount(mDesc.multisample.sampleCount),
        .sampleShadingEnable = mDesc.multisample.enabled,
    };
    Vector<vk::PipelineColorBlendAttachmentState> blend_attachments(alloc.Ptr());
    Vector<vk::Format> color_attachment_formats(alloc.Ptr());
    for (const auto& attachment : mDesc.attachments)
    {
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
            vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA};
        blend_attachments.push_back(blend_attachment);
    }
    vk::PipelineColorBlendStateCreateInfo color_blending{.logicOpEnable = VK_FALSE,
                                                         .logicOp = vk::LogicOp::eCopy, // TODO - logicOp unused for now
                                                         .attachmentCount =
                                                         static_cast<uint32_t>(blend_attachments.size()),
                                                         .pAttachments = blend_attachments.data(),
                                                         .blendConstants = Array<float, 4>{1.0f, 1.0f, 1.0f, 1.0f}};
    vk::DynamicState dynamic_states[2]{vk::DynamicState::eViewport, vk::DynamicState::eScissor};
    vk::PipelineDynamicStateCreateInfo dynamic_state{.dynamicStateCount = 2, // Viewport and Scissor
                                                     .pDynamicStates = dynamic_states};
    vk::PipelineRenderingCreateInfo rendering_create_info{
        .colorAttachmentCount = static_cast<uint32_t>(color_attachment_formats.size()),
        .pColorAttachmentFormats = color_attachment_formats.data(),
        .depthAttachmentFormat = vkFormatFromRHIFormat(mDesc.depthStencil.depthFormat),
        .stencilAttachmentFormat = vkFormatFromRHIFormat(mDesc.depthStencil.stencilFormat),
    };
    Vector<vk::PipelineShaderStageCreateInfo> shaderStages(alloc.Ptr());
    for (auto& shader : mDesc.shaderStages)
    {
        vk::SpecializationInfo* pSpecializationInfo = nullptr;
        if (!shader.desc.specializationData.empty())
        {
            pSpecializationInfo = Construct<vk::SpecializationInfo>(alloc.Ptr());
            auto* entry = Construct<vk::SpecializationMapEntry>(alloc.Ptr());
            entry->constantID = 0, entry->offset = 0, entry->size = shader.desc.specializationData.size_bytes();
            pSpecializationInfo->mapEntryCount = 1;
            pSpecializationInfo->pMapEntries = entry;
            pSpecializationInfo->dataSize = shader.desc.specializationData.size_bytes();
            pSpecializationInfo->pData = shader.desc.specializationData.data();
        }
        shaderStages.push_back({
            .stage = vkFlagsToBits(vkShaderStageFlagsFromRHIShaderStage(shader.desc.stage)),
            .module = static_cast<VulkanShaderModule*>(shader.shaderModule)->GetVkShaderModule(),
            .pName = shader.desc.entryPoint,
            .pSpecializationInfo = pSpecializationInfo
        });
    }
    CHECK_MSG(!shaderStages.empty(), "At least one shader stage must be specified in a graphics pipeline");
    vk::GraphicsPipelineCreateInfo pipelineInfo{.pNext = &rendering_create_info,
                                                .stageCount = static_cast<uint32_t>(shaderStages.size()),
                                                .pStages = shaderStages.data(),
                                                .pVertexInputState = &vtx,
                                                .pInputAssemblyState = &ia,
                                                .pViewportState = &viewport,
                                                .pRasterizationState = &rasterizer,
                                                .pMultisampleState = &multisampling,
                                                .pDepthStencilState = &depth_stencil,
                                                .pColorBlendState = &color_blending,
                                                .pDynamicState = &dynamic_state,
                                                .layout = mPipelineLayout,
                                                .renderPass = nullptr};
    const vk::raii::PipelineCache noCache{nullptr};
    vk::raii::PipelineCache const* pPSOCache{&noCache};
    if (mDesc.psoCache)
    {
        auto* vkCache = static_cast<VulkanPipelineStateCache*>(mDesc.psoCache);
        pPSOCache = &vkCache->GetVkPipelineCache();
    }
    mPipeline = vk::raii::Pipeline(mDevice.GetVkDevice(), *pPSOCache, pipelineInfo, nullptr);
}

void VulkanPipelineState::InitializeCompute()
{
    StackArena<> arena;
    AllocatorStack alloc(arena);
    CHECK_MSG(mDesc.shaderStages.size() == 1, "Compute pipeline must have exactly 1 shader stage.");
    auto const& shader = mDesc.shaderStages[0];
    CHECK_MSG(shader.desc.stage.is_bitmask() && shader.desc.stage == RHIShaderStageBits::Compute,
              "Compute stage must contain only Compute shaders");
    vk::SpecializationInfo* pSpecializationInfo = nullptr;
    if (!shader.desc.specializationData.empty())
    {
        pSpecializationInfo = Construct<vk::SpecializationInfo>(alloc.Ptr());
        auto* entry = Construct<vk::SpecializationMapEntry>(alloc.Ptr());
        entry->constantID = 0, entry->offset = 0, entry->size = shader.desc.specializationData.size_bytes();
        pSpecializationInfo->mapEntryCount = 1;
        pSpecializationInfo->pMapEntries = entry;
        pSpecializationInfo->dataSize = shader.desc.specializationData.size_bytes();
        pSpecializationInfo->pData = shader.desc.specializationData.data();
    }
    vk::PipelineShaderStageCreateInfo stage_info{
        .stage = vk::ShaderStageFlagBits::eCompute,
        .module = static_cast<VulkanShaderModule*>(shader.shaderModule)->GetVkShaderModule(),
        .pName = shader.desc.entryPoint,
        .pSpecializationInfo = pSpecializationInfo
    };
    vk::ComputePipelineCreateInfo pipelineInfo{.stage = stage_info, .layout = mPipelineLayout};
    const vk::raii::PipelineCache noCache{nullptr};
    vk::raii::PipelineCache const* pPSOCache{&noCache};
    if (mDesc.psoCache)
    {
        auto* vkCache = static_cast<VulkanPipelineStateCache*>(mDesc.psoCache);
        pPSOCache = &vkCache->GetVkPipelineCache();
    }
    mPipeline = vk::raii::Pipeline(mDevice.GetVkDevice(), *pPSOCache, pipelineInfo, nullptr);
}

void VulkanPipelineState::InitializeRayTracing()
{
    StackArena<> arena;
    AllocatorStack alloc(arena);
    auto [physProps, rtProps] = mDevice.GetVkPhysicalDevice().getProperties2<
        vk::PhysicalDeviceProperties2, vk::PhysicalDeviceRayTracingPipelinePropertiesKHR>();
    const PipelineStateDesc::ShaderStage* rayGenShader = nullptr;
    Vector<const PipelineStateDesc::ShaderStage*> missShaders(alloc.Ptr());
    Vector<const PipelineStateDesc::ShaderStage*> hitShaders(alloc.Ptr());
    for (auto& shader : mDesc.shaderStages)
    {
        if (shader.desc.stage & RHIShaderStageBits::RayGeneration)
        {
            CHECK_MSG(!rayGenShader, "Only one ray generation shader can be specified per ray tracing pipeline");
            rayGenShader = &shader;
        }
        else if (shader.desc.stage & RHIShaderStageBits::RayMiss)
            missShaders.push_back(&shader);
        else if (shader.desc.stage & (RHIShaderStageBits::RayClosestHit | RHIShaderStageBits::RayAnyHit |
            RHIShaderStageBits::RayIntersection))
            hitShaders.push_back(&shader);
    }
    CHECK_MSG(rayGenShader, "One ray generation shader must be specified per ray tracing pipeline");
    Vector<vk::PipelineShaderStageCreateInfo> stages(alloc.Ptr());
    Vector<vk::RayTracingShaderGroupCreateInfoKHR> groups(alloc.Ptr());
    // This is incredibly obtuse.
    auto AddShaderStage = [&](const PipelineStateDesc::ShaderStage* s) -> uint32_t
    {
        vk::SpecializationInfo* pSpecializationInfo = nullptr;
        if (!s->desc.specializationData.empty())
        {
            pSpecializationInfo = Construct<vk::SpecializationInfo>(alloc.Ptr());
            auto* entry = Construct<vk::SpecializationMapEntry>(alloc.Ptr());
            entry->constantID = 0, entry->offset = 0, entry->size = s->desc.specializationData.size_bytes();
            pSpecializationInfo->mapEntryCount = 1;
            pSpecializationInfo->pMapEntries = entry;
            pSpecializationInfo->dataSize = s->desc.specializationData.size_bytes();
            pSpecializationInfo->pData = s->desc.specializationData.data();
        }
        vk::PipelineShaderStageCreateInfo stageInfo = {
            .stage = vkFlagsToBits(vkShaderStageFlagsFromRHIShaderStage(s->desc.stage)),
            .module = static_cast<VulkanShaderModule*>(s->shaderModule)->GetVkShaderModule(),
            .pName = s->desc.entryPoint,
            .pSpecializationInfo = pSpecializationInfo
        };
        stages.push_back(stageInfo);
        return static_cast<uint32_t>(stages.size() - 1); // Return index
    };
    // RayGen
    {
        uint32_t stageIdx = AddShaderStage(rayGenShader);
        auto& g = groups.emplace_back();
        g.type = vk::RayTracingShaderGroupTypeKHR::eGeneral;
        g.generalShader = stageIdx;
        g.closestHitShader = VK_SHADER_UNUSED_KHR;
        g.anyHitShader = VK_SHADER_UNUSED_KHR;
        g.intersectionShader = VK_SHADER_UNUSED_KHR;
    }
    // Miss
    for (auto* s : missShaders)
    {
        uint32_t stageIdx = AddShaderStage(s);
        auto& g = groups.emplace_back();
        g.type = vk::RayTracingShaderGroupTypeKHR::eGeneral;
        g.generalShader = stageIdx;
        g.closestHitShader = VK_SHADER_UNUSED_KHR;
        g.anyHitShader = VK_SHADER_UNUSED_KHR;
        g.intersectionShader = VK_SHADER_UNUSED_KHR;
    }
    // Hits
    // Needs to be grouped by hit group
    uint32_t hitGroups = 0;
    for (auto* s : hitShaders)
    {
        hitGroups = std::max(hitGroups, s->desc.raytracingHitGroupIndex + 1);
    }
    for (size_t i = 0; i < hitGroups; i++)
        groups.push_back({
            .type = vk::RayTracingShaderGroupTypeKHR::eTrianglesHitGroup,
            .generalShader = VK_SHADER_UNUSED_KHR,
            .closestHitShader = VK_SHADER_UNUSED_KHR,
            .anyHitShader = VK_SHADER_UNUSED_KHR,
            .intersectionShader = VK_SHADER_UNUSED_KHR
        });
    Span<vk::RayTracingShaderGroupCreateInfoKHR> hitSpan(groups);
    hitSpan = hitSpan.subspan(hitSpan.size() - hitGroups, hitGroups);
    for (auto s : hitShaders)
    {
        uint32_t stageIdx = AddShaderStage(s);
        auto& g = hitSpan[s->desc.raytracingHitGroupIndex];
        if (s->desc.stage & RHIShaderStageBits::RayClosestHit)
        {
            CHECK_MSG(g.closestHitShader == VK_SHADER_UNUSED_KHR,
                      "Multiple closest hit shaders specified for hit group {}", s->desc.raytracingHitGroupIndex);
            g.closestHitShader = stageIdx;
        }
        if (s->desc.stage & RHIShaderStageBits::RayAnyHit)
        {
            CHECK_MSG(g.anyHitShader == VK_SHADER_UNUSED_KHR,
                      "Multiple closest hit shaders specified for hit group {}", s->desc.raytracingHitGroupIndex);
            g.anyHitShader = stageIdx;
        }
    }
    vk::RayTracingPipelineCreateInfoKHR pipelineInfo{
        .stageCount = static_cast<uint32_t>(stages.size()),
        .pStages = stages.data(),
        .groupCount = static_cast<uint32_t>(groups.size()),
        .pGroups = groups.data(),
        .maxPipelineRayRecursionDepth = std::min(rtProps.maxRayRecursionDepth, 3u),
        .layout = mPipelineLayout,
    };
    const vk::raii::PipelineCache noCache{nullptr};
    vk::raii::PipelineCache const* pPSOCache{&noCache};
    if (mDesc.psoCache)
    {
        auto* vkCache = static_cast<VulkanPipelineStateCache*>(mDesc.psoCache);
        pPSOCache = &vkCache->GetVkPipelineCache();
    }
    mPipeline = vk::raii::Pipeline(mDevice.GetVkDevice(), nullptr, *pPSOCache,
                                   pipelineInfo,
                                   nullptr);
    const uint32_t handleSize = rtProps.shaderGroupHandleSize;
    const uint32_t handleAlignment = rtProps.shaderGroupHandleAlignment;
    const uint32_t baseAlignment = rtProps.shaderGroupBaseAlignment;
    const uint32_t handleStride = AlignUp(handleSize, handleAlignment);

    uint32_t kRgenCount = 1;
    auto missCount = static_cast<uint32_t>(missShaders.size());
    const auto hitCount = hitGroups;
    mSBT.raygen.stride = mSBT.raygen.size = AlignUp(kRgenCount * handleStride, baseAlignment);
    mSBT.miss.stride = handleStride, mSBT.miss.size = AlignUp(missCount * handleStride, baseAlignment);
    mSBT.hit.stride = handleStride, mSBT.hit.size = AlignUp(hitCount * handleStride, baseAlignment);
    // Create Buffer
    vk::DeviceSize sbtSize = mSBT.raygen.size + mSBT.miss.size + mSBT.hit.size;
    mSBTBuffer = mDevice.CreateBuffer(RHIBufferDesc{
        .resource = {
            .hostAccess = RHIResourceHostAccess::WriteOnly
        },
        .usage = RHIBufferUsageBits::ShaderBindingTable | RHIBufferUsageBits::DeviceAddress,
        .size = sbtSize,
    });
    auto handles = mPipeline.getRayTracingShaderGroupHandlesKHR<uint8_t>(
        0, static_cast<uint32_t>(groups.size()), handleSize * groups.size());
    auto pData = mSBTBuffer->Map<uint8_t>();
    vk::DeviceAddress sbtAddr = static_cast<VulkanBuffer*>(mSBTBuffer.Get())->GetBufferAddress();
    auto CopyHandlesToSBT = [&](uint32_t startGroupIdx, uint32_t count, uint32_t bufferOffset)
    {
        for (uint32_t i = 0; i < count; i++)
        {
            const uint8_t* src = handles.data() + (startGroupIdx + i) * handleSize;
            uint8_t* dst = pData + bufferOffset + (i * handleStride);
            std::memcpy(dst, src, handleSize);
        }
    };
    // RayGen
    mSBT.raygen.deviceAddress = sbtAddr;
    CopyHandlesToSBT(0, kRgenCount, 0);
    // Miss
    uint32_t missOffset = mSBT.raygen.size;
    mSBT.miss.deviceAddress = sbtAddr + missOffset;
    CopyHandlesToSBT(kRgenCount, missCount, missOffset);
    // Hit
    uint32_t hitOffset = missOffset + mSBT.miss.size;
    mSBT.hit.deviceAddress = sbtAddr + hitOffset;
    CopyHandlesToSBT(kRgenCount + missCount, hitCount, hitOffset);
}

VulkanPipelineState::VulkanPipelineState(VulkanDevice& device, PipelineStateDesc const& desc) :
    RHIPipelineState(device, desc), mDevice(device)
{
    InitializePipelineLayout();
    switch (desc.type)
    {
    case RHIDevicePipelineType::Graphics:
        InitializeGraphics();
        break;
    case RHIDevicePipelineType::Compute:
        InitializeCompute();
        break;
    case RHIDevicePipelineType::RayTracing:
        InitializeRayTracing();
        break;
    }
}

void VulkanPipelineState::DebugSetObjectName(const char* name)
{
    VkPipeline handle = *mPipeline;
    mDevice.GetVkDevice().setDebugUtilsObjectNameEXT({.objectType = vk::ObjectType::ePipeline,
                                                      .objectHandle = reinterpret_cast<uint64_t>(handle),
                                                      .pObjectName = name});
}
