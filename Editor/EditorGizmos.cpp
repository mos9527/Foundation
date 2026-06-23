#include "EditorGizmos.hpp"
#include "EditorState.hpp"
#include "Context.hpp"
#include <Core/Paths.hpp>
#include <RenderCore/Bindless.hpp>
#include <RenderCore/ImmediateContext.hpp>
#include <Renderer/Mesh.hpp>
#include <Renderer/Texture.hpp>
#include <cmath>
#include <cfloat>
#include <cstring>
#include <numbers>

using namespace Foundation;
using namespace Foundation::RenderCore;
using Foundation::Core::PathsResolve;

namespace
{
static constexpr int kCircleSegments = 32;
static constexpr float kIconWorldHalfExtent = 1.0f;
static constexpr size_t kMaxIconBindings = 16u;
static constexpr float kGridCellSize = 1.0f;
static constexpr int kGridMajorEvery = 10;
static constexpr float kGridHalfExtent = 100.0f;

struct GizmoVertex
{
    float3 position;
    float4 color;
};

struct GizmoSpriteVertex
{
    float3 center;
    float2 corner;
    float2 uv;
    float4 color;
    uint32_t textureId;
};

struct GizmoUBO
{
    mat4 viewProj{1.0f};
    mat4 view{1.0f};
    float zNear{0.1f};
    float depthBias{0.05f};
    float fadeRange{0.4f};
    float occludedAlpha{0.25f};
    float3 camRight{1.0f, 0.0f, 0.0f};
    float _pad0{};
    float3 camUp{0.0f, 1.0f, 0.0f};
    float _pad1{};
    float iconWorldHalfExtent{kIconWorldHalfExtent};
    float distanceFadeStart{25.0f};
    float distanceFadeEnd{120.0f};
    float3 camPosition{};
    float2 screenSize{};
};

struct LightIconBindings
{
    uint32_t directional{~0u};
    uint32_t point{~0u};
    uint32_t spot{~0u};
    uint32_t disk{~0u};
    uint32_t rect{~0u};
};

struct GizmoState
{
    UniquePtr<BindlessPool> iconPool;
    LightIconBindings icons{};
    ResourceHandle vertexBuffer{kInvalidHandle};
    ResourceHandle spriteVertexBuffer{kInvalidHandle};
    ResourceHandle spriteIndexBuffer{kInvalidHandle};
    ResourceHandle ubo{kInvalidHandle};
    ResourceHandle linearSampler{kInvalidHandle};
    PassHandle linePass{kInvalidHandle};
    PassHandle spritePass{kInvalidHandle};
    Vector<GizmoVertex> vertices{GLOBAL_ALLOC};
    Vector<GizmoSpriteVertex> spriteVertices{GLOBAL_ALLOC};
    Vector<uint16_t> spriteIndices{GLOBAL_ALLOC};
    uint32_t vertexCount{0u};
    uint32_t spriteIndexCount{0u};
};

GizmoState sGizmo;

static int TonemapSpecializationFlags(Renderer* renderer)
{
    int flags{};
    constexpr int kFlagsOutputSrgb = 1 << 0;
    constexpr int kFlagsOutputSt2084 = 1 << 1;
    switch (renderer->GetSwapchain()->mDesc.colorSpace)
    {
    case RHIColorSpace::Hdr10St2084: flags |= kFlagsOutputSt2084; break;
    case RHIColorSpace::SrgbNonLinear:
    default: flags |= kFlagsOutputSrgb; break;
    }
    return flags;
}

static uint32_t UploadIconTexture(BindlessPool& pool, RHIDevice* device, StringView relPath)
{
    FTexture cpu(GLOBAL_ALLOC);
    LoadRGBA8(cpu, PathsResolve(relPath), false);

    auto texture = device->CreateTexture(
        {.usage = RHITextureUsageBits::SampledImage | RHITextureUsageBits::TransferDestination,
         .extent = {cpu.GetWidth(), cpu.GetHeight(), 1u},
         .format = RHIResourceFormat::R8G8B8A8Unorm});
    auto view = texture->CreateTextureView(
        {.format = RHIResourceFormat::R8G8B8A8Unorm, .range = RHITextureSubresourceRange::Create()});

    ImmediateContext im(RHIDeviceQueueType::Graphics, device);
    size_t const uploadBytes = cpu.bytes.size();
    auto staging = device->CreateBuffer(RHIBufferDesc::CreateStagingDesc(uploadBytes));
    std::memcpy(staging->Map(), cpu.bytes.data(), uploadBytes);
    im->Begin();
    im->BeginTransition();
    im->SetImageTransition(texture.Get(),
                           {.dstAccess = RHIResourceAccessBits::TransferWrite,
                            .dstStage = RHIPipelineStageBits::Transfer,
                            .dstImgLayout = RHITextureLayout::TransferDst,
                            .srcImgRange = RHITextureSubresourceRange::Create()});
    im->EndTransition();
    im->CopyBufferToImage(staging.Get(), texture.Get(), RHITextureLayout::TransferDst,
                          {{RHICommandList::CopyImageRegion{
                              .dstLayer = RHITextureSubresourceLayer{
                                  .aspect = RHITextureAspectFlagBits::Color,
                                  .mipLevel = 0,
                                  .baseArrayLayer = 0,
                                  .layerCount = 1,
                              },
                              .extent = {cpu.GetWidth(), cpu.GetHeight(), 1u},
                          }}});
    im->BeginTransition();
    im->SetImageTransition(texture.Get(),
                           {.srcAccess = RHIResourceAccessBits::TransferRead,
                            .dstAccess = RHIResourceAccessBits::ShaderRead,
                            .srcStage = RHIPipelineStageBits::Transfer,
                            .dstStage = RHIPipelineStageBits::FragmentShader,
                            .srcImgLayout = RHITextureLayout::TransferDst,
                            .dstImgLayout = RHITextureLayout::ShaderReadOnly,
                            .srcImgRange = RHITextureSubresourceRange::Create()});
    im->EndTransition();
    im->End();
    im.Submit();
    im.WaitIdle();

    return pool.Allocate(std::move(texture), std::move(view));
}

static void LoadLightIcons(BindlessPool& pool, RHIDevice* device)
{
    sGizmo.icons.directional = UploadIconTexture(pool, device, "Data/Icons/LightDirectional.png");
    sGizmo.icons.point = UploadIconTexture(pool, device, "Data/Icons/LightPoint.png");
    sGizmo.icons.spot = UploadIconTexture(pool, device, "Data/Icons/LightSpot.png");
    sGizmo.icons.disk = UploadIconTexture(pool, device, "Data/Icons/LightAreaDisk.png");
    sGizmo.icons.rect = UploadIconTexture(pool, device, "Data/Icons/LightAreaRect.png");
}

static uint32_t IconForLight(FLightType type, LightIconBindings const& icons)
{
    switch (type)
    {
    case FLightType::Directional: return icons.directional;
    case FLightType::Point:       return icons.point;
    case FLightType::Spot:        return icons.spot;
    case FLightType::Disk:        return icons.disk;
    case FLightType::Rect:        return icons.rect;
    default:                      return ~0u;
    }
}

static void AppendSpriteBillboard(Vector<GizmoSpriteVertex>& vertices, Vector<uint16_t>& indices, float3 center,
                                  float4 color, uint32_t textureId)
{
    uint16_t base = static_cast<uint16_t>(vertices.size());
    vertices.push_back({center, float2{-1.0f, -1.0f}, float2{0.0f, 0.0f}, color, textureId});
    vertices.push_back({center, float2{1.0f, -1.0f}, float2{1.0f, 0.0f}, color, textureId});
    vertices.push_back({center, float2{1.0f, 1.0f}, float2{1.0f, 1.0f}, color, textureId});
    vertices.push_back({center, float2{-1.0f, 1.0f}, float2{0.0f, 1.0f}, color, textureId});
    indices.push_back(base + 0);
    indices.push_back(base + 1);
    indices.push_back(base + 2);
    indices.push_back(base + 0);
    indices.push_back(base + 2);
    indices.push_back(base + 3);
}
static void AppendLine(Vector<GizmoVertex>& vertices, float3 a, float3 b, float4 color)
{
    vertices.push_back({a, color});
    vertices.push_back({b, color});
}

static float4 GridLineColor(float worldCoord, float4 axisColor, float4 majorColor, float4 minorColor)
{
    if (std::abs(worldCoord) < kGridCellSize * 0.5f)
        return axisColor;

    int const lineIndex = static_cast<int>(std::round(worldCoord / kGridCellSize));
    if (lineIndex % kGridMajorEvery == 0)
        return majorColor;
    return minorColor;
}

static void AppendXZGrid(Vector<GizmoVertex>& vertices, float3 origin)
{
    int const cellCount = static_cast<int>(kGridHalfExtent / kGridCellSize);
    float const originX = std::floor(origin.x / kGridCellSize) * kGridCellSize;
    float const originZ = std::floor(origin.z / kGridCellSize) * kGridCellSize;

    float4 const minorColor{0.55f, 0.55f, 0.55f, 0.2f};
    float4 const majorColor{0.65f, 0.65f, 0.65f, 0.35f};
    float4 const xAxisColor{0.85f, 0.25f, 0.25f, 0.55f};
    float4 const zAxisColor{0.25f, 0.45f, 0.85f, 0.55f};

    for (int i = -cellCount; i <= cellCount; ++i)
    {
        float const z = originZ + static_cast<float>(i) * kGridCellSize;
        float4 const rowColor = GridLineColor(z, xAxisColor, majorColor, minorColor);
        AppendLine(vertices, float3{originX - kGridHalfExtent, 0.0f, z}, float3{originX + kGridHalfExtent, 0.0f, z},
                   rowColor);

        float const x = originX + static_cast<float>(i) * kGridCellSize;
        float4 const colColor = GridLineColor(x, zAxisColor, majorColor, minorColor);
        AppendLine(vertices, float3{x, 0.0f, originZ - kGridHalfExtent}, float3{x, 0.0f, originZ + kGridHalfExtent},
                   colColor);
    }
}

static void AppendWireCircle(Vector<GizmoVertex>& vertices, float3 center, float3 u, float3 v, float2 radius,
                             float4 color, int segments = kCircleSegments)
{
    for (int i = 0; i < segments; ++i)
    {
        float a0 = static_cast<float>(i) * (2.0f * std::numbers::pi_v<float> / segments);
        float a1 = static_cast<float>(i + 1) * (2.0f * std::numbers::pi_v<float> / segments);
        float3 p0 = center + u * (std::cos(a0) * radius.x) + v * (std::sin(a0) * radius.y);
        float3 p1 = center + u * (std::cos(a1) * radius.x) + v * (std::sin(a1) * radius.y);
        AppendLine(vertices, p0, p1, color);
    }
}

static void AppendWireSphere(Vector<GizmoVertex>& vertices, float3 center, float radius, float4 color)
{
    AppendWireCircle(vertices, center, float3(1, 0, 0), float3(0, 1, 0), float2(radius), color);
    AppendWireCircle(vertices, center, float3(1, 0, 0), float3(0, 0, 1), float2(radius), color);
    AppendWireCircle(vertices, center, float3(0, 1, 0), float3(0, 0, 1), float2(radius), color);
}

static void AppendDirectionalGizmo(Vector<GizmoVertex>& vertices, FLight const& light, float4 color)
{
    float3 pos = light.transform.transform;
    float3 dir = normalize(light.transform.rotation * float3(0.0f, 0.0f, -1.0f));
    float len = 2.0f;

    AppendLine(vertices, pos, pos + dir * len, color);

    float3 u, v;
    CoordinateSystem(dir, u, v);
    float3 tip = pos + dir * len;
    for (int i = 0; i < 3; ++i)
    {
        float a = static_cast<float>(i) * (2.0f * std::numbers::pi_v<float> / 3.0f);
        float3 base = tip - dir * 0.3f + (u * std::cos(a) + v * std::sin(a)) * 0.15f;
        AppendLine(vertices, tip, base, color);
    }

    AppendWireCircle(vertices, pos, u, v, float2(0.15f), color, 16);

    if (light.angularDiameter > 0.0f)
    {
        float coneLen = 1.5f;
        float outerR = coneLen * std::tan(light.angularDiameter * 0.5f);
        float3 coneBase = pos + dir * coneLen;
        AppendWireCircle(vertices, coneBase, u, v, float2(outerR), color);
        for (int i = 0; i < 4; ++i)
        {
            float a = static_cast<float>(i) * (std::numbers::pi_v<float> * 0.5f);
            float3 edge = coneBase + (u * std::cos(a) + v * std::sin(a)) * outerR;
            AppendLine(vertices, pos, edge, color);
        }
    }
}

static void AppendPointGizmo(Vector<GizmoVertex>& vertices, FLight const& light, float4 color)
{
    float3 pos = light.transform.transform;
    float radius = light.range > 0.0f ? light.range : 0.5f;
    AppendWireSphere(vertices, pos, radius, color);
    AppendWireCircle(vertices, pos, float3(1, 0, 0), float3(0, 1, 0), float2(0.05f), color, 12);
}

static void AppendSpotGizmo(Vector<GizmoVertex>& vertices, FLight const& light, float4 color)
{
    float3 pos = light.transform.transform;
    float3 dir = normalize(light.transform.rotation * float3(0.0f, 0.0f, -1.0f));
    float coneLen = light.range > 0.0f ? light.range : 3.0f;
    float outerR = coneLen * std::tan(light.spotOuterConeAngle);

    float3 u, v;
    CoordinateSystem(dir, u, v);
    float3 tip = pos + dir * coneLen;

    AppendWireCircle(vertices, tip, u, v, float2(outerR), color, 24);
    for (int i = 0; i < 4; ++i)
    {
        float a = static_cast<float>(i) * (std::numbers::pi_v<float> * 0.5f);
        float3 base = tip + (u * std::cos(a) + v * std::sin(a)) * outerR;
        AppendLine(vertices, pos, base, color);
    }

    if (light.spotInnerConeAngle > 0.001f)
    {
        float innerR = coneLen * std::tan(light.spotInnerConeAngle);
        float4 innerColor = color * float4(1.0f, 1.0f, 1.0f, 0.5f);
        AppendWireCircle(vertices, tip, u, v, float2(innerR), innerColor, 24);
    }
}

static void AppendDiskGizmo(Vector<GizmoVertex>& vertices, FLight const& light, float4 color)
{
    float3 pos = light.transform.transform;
    float3 dir = normalize(light.transform.rotation * float3(0.0f, 0.0f, -1.0f));
    float3 u, v;
    CoordinateSystem(dir, u, v);

    AppendWireCircle(vertices, pos, u, v, float2(light.width, light.height), color);
    float arrowLen = std::max(light.width, light.height) * 1.5f;
    AppendLine(vertices, pos, pos + dir * arrowLen, color);
}

static void AppendRectGizmo(Vector<GizmoVertex>& vertices, FLight const& light, float4 color)
{
    float3 pos = light.transform.transform;
    float3 dir = normalize(light.transform.rotation * float3(0.0f, 0.0f, -1.0f));
    float3 u, v;
    CoordinateSystem(dir, u, v);

    float3 corners[4] = {
        pos + u * light.width + v * light.height,
        pos - u * light.width + v * light.height,
        pos - u * light.width - v * light.height,
        pos + u * light.width - v * light.height,
    };
    for (int i = 0; i < 4; ++i)
        AppendLine(vertices, corners[i], corners[(i + 1) % 4], color);

    AppendLine(vertices, pos, pos + dir * 0.5f, color);
}

static float4 GizmoColor(bool selected)
{
    return selected ? float4{1.0f, 0.78f, 0.2f, 1.0f} : float4{1.0f, 1.0f, 0.4f, 0.4f};
}

static float4 LightGizmoSpriteColor(FLight const& light, bool selected)
{
    float3 rgb = max(light.color, float3(0.0f));
    float const maxC = std::max({rgb.x, rgb.y, rgb.z, 1e-6f});
    rgb *= 1.0f / maxC;
    return float4(rgb, selected ? 1.0f : 0.85f);
}

static bool WorldToRenderPixel(float3 world, mat4 const& viewProj, float2 renderSize, float2& outPixel)
{
    float4 clip = viewProj * float4(world, 1.0f);
    if (clip.w <= 1e-6f)
        return false;

    float2 ndc = float2(clip.x, clip.y) / clip.w;
    outPixel.x = (ndc.x * 0.5f + 0.5f) * renderSize.x;
    outPixel.y = (1.0f - ndc.y * 0.5f - 0.5f) * renderSize.y;
    return true;
}
} // namespace

namespace EditorGizmos
{

void InsertPass(Renderer* renderer, ResourceHandle depthTexture, RHIExtent2D extent)
{
    if (!GEditor.gizmo.enabled)
        return;

    CHECK(renderer);

    sGizmo.iconPool = ConstructUnique<BindlessPool>(GLOBAL_ALLOC, renderer->GetDevice(), GLOBAL_ALLOC,
                                      BindlessPool::BindlessPoolDesc{.maxBindings = static_cast<uint32_t>(kMaxIconBindings)});
    LoadLightIcons(*sGizmo.iconPool, renderer->GetDevice());

    static constexpr size_t kMaxVertexBytes = 8u * 1024u * 1024u;
    static constexpr size_t kMaxSpriteVertexBytes = 512u * 1024u;
    static constexpr size_t kMaxSpriteIndexBytes = 256u * 1024u;
    sGizmo.vertexBuffer = renderer->CreateResource(
        "Editor Gizmo VB",
        RHIBufferDesc{.resource = {.heap = RHIDeviceHeapType::Upload, .hostAccess = RHIResourceHostAccess::WriteOnly},
                      .usage = RHIBufferUsageBits::VertexBuffer | RHIBufferUsageBits::TransferDestination,
                      .size = kMaxVertexBytes});
    sGizmo.spriteVertexBuffer = renderer->CreateResource(
        "Editor Gizmo Sprite VB",
        RHIBufferDesc{.resource = {.heap = RHIDeviceHeapType::Upload, .hostAccess = RHIResourceHostAccess::WriteOnly},
                      .usage = RHIBufferUsageBits::VertexBuffer | RHIBufferUsageBits::TransferDestination,
                      .size = kMaxSpriteVertexBytes});
    sGizmo.spriteIndexBuffer = renderer->CreateResource(
        "Editor Gizmo Sprite IB",
        RHIBufferDesc{.resource = {.heap = RHIDeviceHeapType::Upload, .hostAccess = RHIResourceHostAccess::WriteOnly},
                      .usage = RHIBufferUsageBits::IndexBuffer | RHIBufferUsageBits::TransferDestination,
                      .size = kMaxSpriteIndexBytes});
    sGizmo.ubo = renderer->CreateResource(
        "Editor Gizmo UBO",
        RHIBufferDesc{.usage = RHIBufferUsageBits::TransferDestination | RHIBufferUsageBits::UniformBuffer,
                      .size = sizeof(GizmoUBO)});
    sGizmo.linearSampler = renderer->CreateSampler({});

    renderer->CreatePass(
        "Editor Gizmo UBO Update", RHIDeviceQueueType::Graphics, 0u,
        [=](PassHandle self, Renderer* r) { r->BindBufferCopyDst(self, sGizmo.ubo); },
        [=](PassHandle, Renderer* r, RHICommandList* cmd)
        {
            GizmoUBO params{};
            params.viewProj = GEditor.camera.proj * GEditor.camera.view;
            params.view = GEditor.camera.view;
            params.zNear = GEditor.camera.zNear;
            params.camRight = GEditor.camera.rot * float3(1.0f, 0.0f, 0.0f);
            params.camUp = GEditor.camera.rot * float3(0.0f, 1.0f, 0.0f);
            params.iconWorldHalfExtent = kIconWorldHalfExtent;
            params.camPosition = GEditor.camera.position;
            params.distanceFadeStart = kGridHalfExtent * 0.25f;
            params.distanceFadeEnd = kGridHalfExtent;
            params.screenSize = float2(extent.x, extent.y);
            auto* ubo = r->DerefResource(sGizmo.ubo).Get<RHIBuffer*>();
            cmd->UpdateBuffer(ubo, 0, AsBytes(AsSpan(params)));
        });

    sGizmo.linePass = renderer->CreatePass(
        "Editor Gizmos", RHIDeviceQueueType::Graphics, 0u,
        [=](PassHandle self, Renderer* r)
        {
            r->BindBackbufferRTV(self, RHIPipelineState::PipelineStateDesc::Attachment::Blending::GetAlphaBlending());
            r->BindBufferShaderRead(self, sGizmo.vertexBuffer, RHIPipelineStageBits::VertexShader);
            r->BindVertexInput(
                self,
                {.bindings = {{{sizeof(GizmoVertex), false}}},
                 .attributes = {{
                     {.location = 0, .offset = offsetof(GizmoVertex, position), .format = RHIResourceFormat::R32G32B32SignedFloat},
                     {.location = 1, .offset = offsetof(GizmoVertex, color), .format = RHIResourceFormat::R32G32B32A32SignedFloat},
                 }}});
            r->BindBufferUniform(self, sGizmo.ubo,
                                 RHIPipelineStageBits::VertexShader | RHIPipelineStageBits::FragmentShader,
                                 "globalParams");
            r->BindShader(self, RHIShaderStageBits::Vertex, "vertMain",
                          PathsResolve("Data/Shaders/Editor/EditorGizmo.spv"));
            r->BindShader(self, RHIShaderStageBits::Fragment, "fragMain",
                          PathsResolve("Data/Shaders/Editor/EditorGizmo.spv"));
            r->BindTextureSampler(self, sGizmo.linearSampler, "linSampler");
            if (depthTexture != kInvalidHandle)
            {
                r->BindTextureSRV(self, depthTexture, "sceneDepth", RHIPipelineStageBits::FragmentShader,
                                  RHITextureViewDesc{.format = RHIResourceFormat::R32SignedFloat,
                                                     .range = RHITextureSubresourceRange::Create(
                                                         RHITextureAspectFlagBits::Color)});
            }
            r->PassSetTopology(self, RHIPipelineState::PipelineStateDesc::LineList);
            r->PassSetRasterizerFlags(self, {.cullMode = RHIPipelineState::PipelineStateDesc::Rasterizer::CullNone},
                                      {.depthTest = false, .depthWrite = false});
        },
        [=](PassHandle self, Renderer* r, RHICommandList* cmd)
        {
            if (sGizmo.vertexCount == 0u)
                return;

            auto* vb = r->DerefResource(sGizmo.vertexBuffer).Get<RHIBuffer*>();
            Span<const GizmoVertex> vertexData(sGizmo.vertices.data(), sGizmo.vertexCount);
            cmd->UpdateBuffer(vb, 0, AsBytes(vertexData));

            r->CmdSetPipeline(self, cmd);
            r->CmdBeginGraphics(self, cmd, extent, {{{RHIAttachmentLoadOp::Load}}});
            cmd->SetViewport(0, 0, extent.x, extent.y, 0.0f, 1.0f, true)
                .SetScissor(0, 0, extent.x, extent.y)
                .BindVertexBuffer(0, {{vb}}, {{0u}})
                .Draw(sGizmo.vertexCount);
            cmd->EndGraphics();
        });

    sGizmo.spritePass = renderer->CreatePass(
        "Editor Gizmo Sprites", RHIDeviceQueueType::Graphics, 0u,
        [=](PassHandle self, Renderer* r)
        {
            int const flags = TonemapSpecializationFlags(r);
            r->BindBackbufferRTV(self, RHIPipelineState::PipelineStateDesc::Attachment::Blending::GetAlphaBlending());
            r->BindBufferShaderRead(self, sGizmo.spriteVertexBuffer, RHIPipelineStageBits::VertexShader);
            r->BindBufferShaderRead(self, sGizmo.spriteIndexBuffer, RHIPipelineStageBits::VertexShader);
            r->BindVertexInput(
                self,
                {.bindings = {{{sizeof(GizmoSpriteVertex), false}}},
                 .attributes = {{
                     {.location = 0, .offset = offsetof(GizmoSpriteVertex, center), .format = RHIResourceFormat::R32G32B32SignedFloat},
                     {.location = 1, .offset = offsetof(GizmoSpriteVertex, corner), .format = RHIResourceFormat::R32G32SignedFloat},
                     {.location = 2, .offset = offsetof(GizmoSpriteVertex, uv), .format = RHIResourceFormat::R32G32SignedFloat},
                     {.location = 3, .offset = offsetof(GizmoSpriteVertex, color), .format = RHIResourceFormat::R32G32B32A32SignedFloat},
                     {.location = 4, .offset = offsetof(GizmoSpriteVertex, textureId), .format = RHIResourceFormat::R32Uint},
                 }}});
            r->BindBufferUniform(self, sGizmo.ubo,
                                 RHIPipelineStageBits::VertexShader | RHIPipelineStageBits::FragmentShader,
                                 "globalParams");
            r->BindShader(self, RHIShaderStageBits::Vertex, "vertMain",
                          PathsResolve("Data/Shaders/Editor/EditorGizmoSprite.spv"), AsBytes(AsSpan(flags)));
            r->BindShader(self, RHIShaderStageBits::Fragment, "fragMain",
                          PathsResolve("Data/Shaders/Editor/EditorGizmoSprite.spv"), AsBytes(AsSpan(flags)));
            r->BindDescriptorSet(self, "textures", sGizmo.iconPool->GetDescriptorSetLayout());
            r->BindTextureSampler(self, sGizmo.linearSampler, "linSampler");
            if (depthTexture != kInvalidHandle)
            {
                r->BindTextureSRV(self, depthTexture, "sceneDepth", RHIPipelineStageBits::FragmentShader,
                                  RHITextureViewDesc{.format = RHIResourceFormat::R32SignedFloat,
                                                     .range = RHITextureSubresourceRange::Create(
                                                         RHITextureAspectFlagBits::Color)});
            }
            r->PassSetRasterizerFlags(self, {.cullMode = RHIPipelineState::PipelineStateDesc::Rasterizer::CullNone},
                                      {.depthTest = false, .depthWrite = false});
        },
        [=](PassHandle self, Renderer* r, RHICommandList* cmd)
        {
            if (sGizmo.spriteIndexCount == 0u)
                return;

            auto* vb = r->DerefResource(sGizmo.spriteVertexBuffer).Get<RHIBuffer*>();
            auto* ib = r->DerefResource(sGizmo.spriteIndexBuffer).Get<RHIBuffer*>();
            Span<const GizmoSpriteVertex> vertexData(sGizmo.spriteVertices.data(), sGizmo.spriteVertices.size());
            Span<const uint16_t> indexData(sGizmo.spriteIndices.data(), sGizmo.spriteIndices.size());
            cmd->UpdateBuffer(vb, 0, AsBytes(vertexData)).UpdateBuffer(ib, 0, AsBytes(indexData));

            r->CmdSetPipeline(self, cmd);
            r->CmdBindDescriptorSet(self, cmd, "textures", sGizmo.iconPool->GetDescriptorSet());
            r->CmdBeginGraphics(self, cmd, extent, {{{RHIAttachmentLoadOp::Load}}});
            cmd->SetViewport(0, 0, extent.x, extent.y, 0.0f, 1.0f, true)
                .SetScissor(0, 0, extent.x, extent.y)
                .BindVertexBuffer(0, {{vb}}, {{0u}})
                .BindIndexBuffer(ib, 0, RHIResourceFormat::R16Uint)
                .DrawIndexed(sGizmo.spriteIndexCount);
            cmd->EndGraphics();
        });
}

void BuildLightGizmos()
{
    sGizmo.vertices.clear();
    sGizmo.spriteVertices.clear();
    sGizmo.spriteIndices.clear();
    sGizmo.vertexCount = 0u;
    sGizmo.spriteIndexCount = 0u;

    if (!GEditor.showImGui || !GEditor.HasScene())
        return;

    AppendXZGrid(sGizmo.vertices, GEditor.camera.center);

    auto lights = GEditor.Scene().GetLights();
    for (int i = 0; i < static_cast<int>(lights.size()); ++i)
    {
        FLight const& light = lights[i];
        if (light.type == FLightType::Environment)
            continue;

        bool const selected = i == GEditor.selectedLight;
        float4 const color = GizmoColor(selected);
        switch (light.type)
        {
        case FLightType::Directional: AppendDirectionalGizmo(sGizmo.vertices, light, color); break;
        case FLightType::Point:       AppendPointGizmo(sGizmo.vertices, light, color);       break;
        case FLightType::Spot:        AppendSpotGizmo(sGizmo.vertices, light, color);        break;
        case FLightType::Disk:        AppendDiskGizmo(sGizmo.vertices, light, color);        break;
        case FLightType::Rect:        AppendRectGizmo(sGizmo.vertices, light, color);        break;
        default: break;
        }

        uint32_t const textureId = IconForLight(light.type, sGizmo.icons);
        if (textureId != ~0u)
        {
            float3 const pos = light.transform.transform;
            float4 const spriteColor = LightGizmoSpriteColor(light, selected);
            AppendSpriteBillboard(sGizmo.spriteVertices, sGizmo.spriteIndices, pos, spriteColor, textureId);
        }
    }

    sGizmo.vertexCount = static_cast<uint32_t>(sGizmo.vertices.size());
    sGizmo.spriteIndexCount = static_cast<uint32_t>(sGizmo.spriteIndices.size());
}

int PickLightAtRenderPixel(Math::int2 pixel)
{
    if (!GEditor.showImGui || !GEditor.gizmo.enabled || !GEditor.HasScene())
        return -1;

    RHIExtent2D const renderExtent = GEditor.viewport.renderExtent;
    if (renderExtent.x == 0u || renderExtent.y == 0u)
        return -1;

    float2 const renderSize(static_cast<float>(renderExtent.x), static_cast<float>(renderExtent.y));
    float2 const pickPoint(static_cast<float>(pixel.x) + 0.5f, static_cast<float>(pixel.y) + 0.5f);
    mat4 const viewProj = GEditor.camera.proj * GEditor.camera.view;
    mat4 const view = GEditor.camera.view;
    float3 const camRight = GEditor.camera.rot * float3(1.0f, 0.0f, 0.0f);
    float3 const camUp = GEditor.camera.rot * float3(0.0f, 1.0f, 0.0f);

    int bestLight = -1;
    float bestViewDepth = FLT_MAX;

    auto lights = GEditor.Scene().GetLights();
    for (int i = 0; i < static_cast<int>(lights.size()); ++i)
    {
        FLight const& light = lights[i];
        if (light.type == FLightType::Environment)
            continue;
        if (IconForLight(light.type, sGizmo.icons) == ~0u)
            continue;

        float3 const center = light.transform.transform;
        float3 const corners[4] = {
            center + (-camRight - camUp) * kIconWorldHalfExtent,
            center + (camRight - camUp) * kIconWorldHalfExtent,
            center + (camRight + camUp) * kIconWorldHalfExtent,
            center + (-camRight + camUp) * kIconWorldHalfExtent,
        };

        float minX = FLT_MAX;
        float minY = FLT_MAX;
        float maxX = -FLT_MAX;
        float maxY = -FLT_MAX;
        bool anyVisible = false;
        for (float3 const& corner : corners)
        {
            float2 screen{};
            if (!WorldToRenderPixel(corner, viewProj, renderSize, screen))
                continue;
            anyVisible = true;
            minX = std::min(minX, screen.x);
            minY = std::min(minY, screen.y);
            maxX = std::max(maxX, screen.x);
            maxY = std::max(maxY, screen.y);
        }
        if (!anyVisible)
            continue;

        if (pickPoint.x < minX || pickPoint.x > maxX || pickPoint.y < minY || pickPoint.y > maxY)
            continue;

        float4 const viewPos = view * float4(center, 1.0f);
        float const viewDepth = -viewPos.z;
        if (viewDepth < bestViewDepth)
        {
            bestViewDepth = viewDepth;
            bestLight = i;
        }
    }

    return bestLight;
}

void Shutdown() { sGizmo.iconPool.reset(); }

} // namespace EditorGizmos
