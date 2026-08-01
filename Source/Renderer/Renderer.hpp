#pragma once
#include <Math/Math.hpp>
#include <RenderCore/Renderer.hpp>
#include "GPUScene.hpp"
#include "Shaders/Flags.h"
using namespace Foundation;
using namespace Foundation::Math;
using namespace Foundation::RenderCore;
#pragma pack(push, 4)
struct RendererUBO
{
    uint32_t frameNumber;
    GSOffsetCount instances;
    GSOffsetCount materials;
    float lodThreshold{0.005f};
    float zNear;
    float4x4 view;
    float4x4 proj;
    float4x4 inverseView;
    float4x4 inverseViewProj;
    float4x4 previousViewProj;
    uint32_t cameraHistoryFrame{UINT32_MAX};
    float4 projPlanes; // ij:left, kl:top
    float aperture{0.0f}; // Aperture radius in world units
    float focalDistance{1e1};
    uint32_t apertureBlades{0u};
    float apertureRotation{0.0f};
    float apertureRatio{1.0f};
    uint32_t cameraProjection{0u};
    // -- Framebuffers
    // Note: ALWAYS updated by Renderers themselves (PT/RASTER)
    float fbWidth;
    float fbHeight;
    uint32_t hizLevels;
    uint32_t hizWidth;
    uint32_t hizHeight;
    // -- Lighting
    float camEV{0.0f};
    float4 camPosition;
    float4 camDirection;
    uint32_t ggxLutEIndex{UINT32_MAX};
    uint32_t ggxLutEavgIndex{UINT32_MAX};
    uint32_t ggxLutEIORIndex{UINT32_MAX};
    uint32_t ggxLutEIORavgIndex{UINT32_MAX};
    uint32_t ggxLutEIORInvIndex{UINT32_MAX};
    uint32_t ggxLutEIORInvavgIndex{UINT32_MAX};
    uint32_t sheenLtcIndex{UINT32_MAX};
    uint32_t envMapTextureIndex{UINT32_MAX};
    uint32_t envMapMarginalCDFIndex{UINT32_MAX};
    uint32_t envMapConditionalCDFIndex{UINT32_MAX};
    uint32_t envMapPrefilteredMips{0u};
    uint32_t hasEnvMap{0u};
    float3 envSHCoeffs[9]{};
    uint32_t matcapTextureIndex{UINT32_MAX};
    // Scene lights
    GSOffsetCount lights{};
    GSOffsetCount emissiveClusters{};
    GSOffsetCount lightBVHNodes{};
    GSOffsetCount lightBVHLightIndices{};
    uint32_t firstLightBVHBitmask{0u};
    GSOffsetCount lightBVHGlobalIndices{};
    uint32_t lightBVHValid{0u};
    GSOffsetCount lightBVHDistantNodes{};
    uint32_t energyCompensation{1u};
    // -- Path Tracing
    uint32_t ptAccumulatedFrames{0u};
    uint32_t ptMaxBounces{4u};
    float ptFireflyClamp{1.0f}; // 10^x
    uint32_t ptSamplesPerPixel{1u}; // Always >= 1.
    uint32_t ptPrimaryLightVisibility{0u}; // Analytic lights + sun disks on bounce 0
    float4 sharcPreviousCameraPosition{};
    uint32_t sharcEntries{0u};
    float sharcSceneScale{50.0f};
    float sharcRadianceScale{1000.0f};
    uint32_t sharcAccumulationFrames{20u};
    uint32_t sharcStaleFrames{60u};
    float sharcRoughnessThreshold{0.4f};
    uint32_t sharcEnabled{0u};
    uint32_t sharcUpdateDownscale{5u};
    // -- Debug
    uint32_t dbgShowOutline{0u};
    uint32_t dbgViewFlags{0u};
    uint32_t dbgMaterialFlags{0u};
    uint32_t adaptiveMinSamples{32u};
    float adaptiveThreshold{0.10f}; // 0 = disabled
    float rasterRTShadowBias{0.01f};
};

struct PostprocessUBO
{
    float camEV{0.0f};
    uint32_t viewLutIndex{UINT32_MAX};
    uint32_t dbgShowOutline{0u};
    uint32_t ptAccumulatedFrames{0u};
    float fbWidth{1.0f};
    float fbHeight{1.0f};
    uint32_t outlineInstanceId{~0u};
    float renderWidth{1.0f}; // Internal render target width
    float renderHeight{1.0f}; // Internal render target height
    uint32_t dbgViewFlags{0u};
};

#pragma pack(pop)

inline void UpdateRendererCameraUBO(RendererUBO& ubo, uint32_t frameNumber, float4x4 const& view, float4x4 const& proj)
{
    if (ubo.cameraHistoryFrame != frameNumber)
    {
        ubo.sharcPreviousCameraPosition = ubo.camPosition;
        ubo.previousViewProj = ubo.cameraHistoryFrame == UINT32_MAX ? proj * view : ubo.proj * ubo.view;
        ubo.cameraHistoryFrame = frameNumber;
    }
    ubo.frameNumber = frameNumber;
    ubo.view = view;
    ubo.proj = proj;
    ubo.inverseView = inverse(view);
    ubo.inverseViewProj = inverse(proj * view);
}


struct RendererConfig
{
    ViewFlags viewFlags{ViewFlagsBits::EnableRasterRTShadows};
    MaterialFlags materialFlags{0u};
    CullFlags cullFlags{CullFlagsBits::Frustum | CullFlagsBits::Occlusion | CullFlagsBits::Backface};
    RHIExtent2D renderExtent{0u, 0u};
    PTSampler ptSampler{PTSampler::Sobol};
    LightSampler lightSamplerMode{LightSampler::BVH};
    bool const* ptRenderPaused{nullptr};
    bool ptShaderExecutionReordering{true};
    bool forceTextureLOD0{false};
    bool energyCompensation{true};
    bool ptPrimaryLightVisibility{false};
    bool ptSharc{true};
    uint32_t ptSharcEntries{1u << 22u};
    float ptSharcSceneScale{50.0f};
    uint32_t ptSharcAccumulationFrames{20u};
    uint32_t ptSharcStaleFrames{60u};
    float ptSharcRoughnessThreshold{0.4f};
    uint32_t ptSharcUpdateDownscale{5u};
    bool textureAnisoEnable{true};
    float textureAnisoLevel{16.0f};
    bool textureTrilinear{true};
    bool isRendering{false};
};

inline RHIDeviceSampler::SamplerDesc MakeTextureSamplerDesc(RendererConfig const& cfg)
{
    using SamplerDesc = RHIDeviceSampler::SamplerDesc;
    using MipmapMode = SamplerDesc::Mipmap::MipmapMode;
    return {
        .anisotropy = {.enable = cfg.textureAnisoEnable, .maxLevel = cfg.textureAnisoLevel},
        .mipmap = {.mipmapMode = cfg.textureTrilinear ? MipmapMode::Linear : MipmapMode::Nearest},
        .lod = {.max = 16.0f},
    };
}

struct RendererResources
{
    GPUScene* scene{nullptr};
    ResourceHandle primitiveBuffer{kInvalidHandle};
    ResourceHandle dynamicPrimitiveBuffer{kInvalidHandle};
    ResourceHandle dynamicStagingBuffer{kInvalidHandle};
    ResourceHandle instanceBuffer{kInvalidHandle};
    ResourceHandle materialBuffer{kInvalidHandle};
    ResourceHandle lightBuffer{kInvalidHandle};
    ResourceHandle emissiveClusterBuffer{kInvalidHandle};
    ResourceHandle lightBVHNodeBuffer{kInvalidHandle};
    ResourceHandle lightBVHLightIndexBuffer{kInvalidHandle};
    ResourceHandle lightBVHBitmaskBuffer{kInvalidHandle};
    ResourceHandle lightBVHNodeIndexBuffer{kInvalidHandle};
    ResourceHandle sobolMatricesBuffer{kInvalidHandle};
    ResourceHandle tlas{kInvalidHandle};
    BindlessPool* textures2D{nullptr};
    BindlessPool* textures3D{nullptr};
    RHIBuffer* primitiveBufferRHI{nullptr};
    RHIBuffer* dynamicPrimitiveBufferRHI{nullptr};
    bool hasDynamicGeometry{false};
    bool hasDynamicTextures{false};
    bool hasCurveGeometry{false};
};

[[nodiscard]] RendererResources CreateGPUSceneRendererResources(Renderer* renderer, GPUScene* scene);

void BuildGPUSceneHostUpdatePass(Renderer* renderer, RendererResources& resources);
void BuildGPUSceneAccelerationStructureUpdatePass(Renderer* renderer, RendererResources& resources);
;
void BuildGPUSceneLightBVHRefitPasses(Renderer* renderer, RendererResources& resources, ResourceHandle ubo);

template <typename FSetup, typename FRecord>
PassHandle BuildGPUSceneTextures2DUpdatePass(Renderer* renderer, StringView name, RendererResources const& resources,
                                             FSetup&& setup, FRecord&& record)
{
    CHECK(renderer);
    CHECK(resources.scene);
    PassHandle preTransition = renderer->CreatePass(
        Format("{} Pre-Transition", name), RHIDeviceQueueType::Graphics, 100u, FSetupDefault{},
        [resources](PassHandle, Renderer*, RHICommandList* cmd)
        { resources.scene->BeginDynamicTextureGPU(cmd, false, GPUScene::DynamicTextureGPUAccess::UAV); });
    PassHandle update = renderer->CreatePass(
        name, RHIDeviceQueueType::Compute, 100u,
        [resources, preTransition, setup = std::forward<FSetup>(setup)](PassHandle self, Renderer* r) mutable
        {
            r->BindPass(self, preTransition);
            r->BindDescriptorSetWrite(self, "gStorageTextures2D",
                                      resources.textures2D->GetStorageDescriptorSetLayout(),
                                      resources.textures2D->GetDescriptorSetLayout());
            setup(self, r);
        },
        [resources, record = std::forward<FRecord>(record)](PassHandle self, Renderer* r, RHICommandList* cmd) mutable
        {
            r->CmdBindDescriptorSet(self, cmd, "gStorageTextures2D", resources.textures2D->GetStorageDescriptorSet());
            record(self, r, cmd);
        });
    return renderer->CreatePass(
        Format("{} Post-Transition", name), RHIDeviceQueueType::Graphics, 100u,
        [resources, update](PassHandle self, Renderer* r)
        {
            r->BindPass(self, update);
            r->BindDescriptorSetWrite(self, resources.textures2D->GetStorageDescriptorSetLayout(),
                                      resources.textures2D->GetDescriptorSetLayout());
        },
        [resources](PassHandle, Renderer*, RHICommandList* cmd)
        { resources.scene->EndDynamicTextureGPU(cmd, false, GPUScene::DynamicTextureGPUAccess::UAV); });
}

template <typename FSetup, typename FRecord>
PassHandle BuildGPUSceneTextures3DUpdatePass(Renderer* renderer, StringView name, RendererResources const& resources,
                                             FSetup&& setup, FRecord&& record)
{
    CHECK(renderer);
    CHECK(resources.scene);
    PassHandle preTransition = renderer->CreatePass(
        Format("{} Pre-Transition", name), RHIDeviceQueueType::Graphics, 100u, FSetupDefault{},
        [resources](PassHandle, Renderer*, RHICommandList* cmd)
        { resources.scene->BeginDynamicTextureGPU(cmd, true, GPUScene::DynamicTextureGPUAccess::UAV); });
    PassHandle update = renderer->CreatePass(
        name, RHIDeviceQueueType::Compute, 100u,
        [resources, preTransition, setup = std::forward<FSetup>(setup)](PassHandle self, Renderer* r) mutable
        {
            r->BindPass(self, preTransition);
            r->BindDescriptorSetWrite(self, "gStorageTextures3D",
                                      resources.textures3D->GetStorageDescriptorSetLayout(),
                                      resources.textures3D->GetDescriptorSetLayout());
            setup(self, r);
        },
        [resources, record = std::forward<FRecord>(record)](PassHandle self, Renderer* r, RHICommandList* cmd) mutable
        {
            r->CmdBindDescriptorSet(self, cmd, "gStorageTextures3D", resources.textures3D->GetStorageDescriptorSet());
            record(self, r, cmd);
        });
    return renderer->CreatePass(
        Format("{} Post-Transition", name), RHIDeviceQueueType::Graphics, 100u,
        [resources, update](PassHandle self, Renderer* r)
        {
            r->BindPass(self, update);
            r->BindDescriptorSetWrite(self, resources.textures3D->GetStorageDescriptorSetLayout(),
                                      resources.textures3D->GetDescriptorSetLayout());
        },
        [resources](PassHandle, Renderer*, RHICommandList* cmd)
        { resources.scene->EndDynamicTextureGPU(cmd, true, GPUScene::DynamicTextureGPUAccess::UAV); });
}

template <typename FSetup, typename FRecord>
PassHandle BuildGPUSceneRenderToTexturePass(Renderer* renderer, StringView name, RendererResources const& resources,
                                            TextureHandle target, FSetup&& setup, FRecord&& record)
{
    CHECK(renderer);
    CHECK(resources.scene);
    CHECK(resources.textures2D);
    RHITexture* texture =
        resources.scene->ResolveDynamicTextureGPU(target, RHITextureUsageBits::RenderTarget);
    CHECK(texture);
    ResourceHandle const targetResource = renderer->CreateResource(Format("{} Target", name), texture);
    RHITextureViewDesc const targetView{
        .format = texture->mDesc.format,
        .dimension = texture->mDesc.dimension,
        .range = RHITextureSubresourceRange::Create(RHITextureAspectFlagBits::Color, 0, texture->mDesc.mipLevels, 0,
                                                    texture->mDesc.arrayLayers)};
    PassHandle const update = renderer->CreatePass(
        name, RHIDeviceQueueType::Graphics, 100u,
        [resources, targetResource, targetView, setup = std::forward<FSetup>(setup)](PassHandle self,
                                                                                    Renderer* r) mutable
        {
            r->BindDescriptorSetWrite(self, resources.textures2D->GetDescriptorSetLayout());
            r->BindTextureRTV(self, targetResource, targetView);
            setup(self, r);
        },
        [record = std::forward<FRecord>(record)](PassHandle self, Renderer* r, RHICommandList* cmd) mutable
        { record(self, r, cmd); });
    return renderer->CreatePass(
        Format("{} Post-Transition", name), RHIDeviceQueueType::Graphics, 100u,
        [resources, targetResource, texture, update](PassHandle self, Renderer* r)
        {
            r->BindPass(self, update);
            r->BindTextureShaderRead(
                self, targetResource,
                RHIPipelineStageBits::AllGraphics | RHIPipelineStageBits::ComputeShader |
                    RHIPipelineStageBits::RayTracingShader,
                RHITextureSubresourceRange::Create(RHITextureAspectFlagBits::Color, 0, texture->mDesc.mipLevels, 0,
                                                   texture->mDesc.arrayLayers));
            r->BindDescriptorSetWrite(self, resources.textures2D->GetDescriptorSetLayout());
            r->MakePassUncullable(self);
        },
        [resources, target](PassHandle, Renderer*, RHICommandList*)
        { resources.scene->CompleteDynamicTextureGPU(target, GPUScene::DynamicTextureGPUAccess::RTV); });
}

struct RendererOutputs
{
    RHIExtent2D extent{0u, 0u};
    RHIResourceFormat aovFormat{RHIResourceFormat::R16G16B16A16SignedFloat};
    // Unified pre-postprocess outputs.
    ResourceHandle diffuse{kInvalidHandle};
    ResourceHandle specular{kInvalidHandle};
    ResourceHandle depth{kInvalidHandle};
    ResourceHandle instanceID{kInvalidHandle};
    ResourceHandle debugOutput{kInvalidHandle};
};
