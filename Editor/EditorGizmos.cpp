#include "EditorGizmos.hpp"
#include "EditorState.hpp"
#include <Core/Paths.hpp>
#include <Renderer/Mesh.hpp>
#include <cmath>
#include <numbers>

using namespace Foundation;
using namespace Foundation::RenderCore;
using Foundation::Core::PathsResolve;

namespace
{
struct GizmoVertex
{
    float3 position;
    float4 color;
};

struct GizmoPushConstant
{
    mat4 viewProj{1.0f};
    float depthBias{0.0002f};
    float fadeRange{0.02f};
    float occludedAlpha{0.25f};
    float _pad{};
};

struct GizmoState
{
    ResourceHandle vertexBuffer{kInvalidHandle};
    PassHandle pass{kInvalidHandle};
    Vector<GizmoVertex> vertices{GLOBAL_ALLOC};
    uint32_t vertexCount{0u};
};

GizmoState sGizmo;

static constexpr float kLineWidth = 0.02f;
static constexpr int kCircleSegments = 32;

static void AppendTriangle(Vector<GizmoVertex>& vertices, float3 p0, float3 p1, float3 p2, float4 color)
{
    vertices.push_back({p0, color});
    vertices.push_back({p1, color});
    vertices.push_back({p2, color});
}

static float3 LineSide(float3 dir)
{
    if (std::abs(dir.y) < 0.99f)
        return normalize(cross(dir, float3(0.0f, 1.0f, 0.0f)));
    return normalize(cross(dir, float3(1.0f, 0.0f, 0.0f)));
}

static void AppendLine(Vector<GizmoVertex>& vertices, float3 a, float3 b, float4 color, float width = kLineWidth)
{
    float3 dir = b - a;
    float len = length(dir);
    if (len < 1e-6f)
        return;
    dir /= len;
    float3 side = LineSide(dir) * (width * 0.5f);
    AppendTriangle(vertices, a - side, a + side, b + side, color);
    AppendTriangle(vertices, a - side, b + side, b - side, color);
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

    AppendLine(vertices, pos, pos + dir * len, color, kLineWidth * 1.5f);

    float3 u, v;
    CoordinateSystem(dir, u, v);
    float3 tip = pos + dir * len;
    for (int i = 0; i < 3; ++i)
    {
        float a = static_cast<float>(i) * (2.0f * std::numbers::pi_v<float> / 3.0f);
        float3 base = tip - dir * 0.3f + (u * std::cos(a) + v * std::sin(a)) * 0.15f;
        AppendLine(vertices, tip, base, color, kLineWidth * 1.5f);
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
    AppendLine(vertices, pos, pos + dir * arrowLen, color, kLineWidth * 1.5f);
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

    AppendLine(vertices, pos, pos + dir * 0.5f, color, kLineWidth * 1.5f);
}

static float4 GizmoColor(bool selected)
{
    return selected ? float4{1.0f, 0.78f, 0.2f, 1.0f} : float4{1.0f, 1.0f, 0.4f, 0.4f};
}
} // namespace

namespace EditorGizmos
{

void InsertPass(Renderer* renderer, ResourceHandle depthTexture, RHIExtent2D extent)
{
    CHECK(renderer);

    static constexpr size_t kMaxVertexBytes = 8u * 1024u * 1024u;
    sGizmo.vertexBuffer = renderer->CreateResource(
        "Editor Gizmo VB",
        RHIBufferDesc{.resource = {.heap = RHIDeviceHeapType::Upload, .hostAccess = RHIResourceHostAccess::WriteOnly},
                      .usage = RHIBufferUsageBits::VertexBuffer | RHIBufferUsageBits::TransferDestination,
                      .size = kMaxVertexBytes});

    auto const blending = RHIPipelineState::PipelineStateDesc::Attachment::Blending::GetAlphaBlending();
    sGizmo.pass = renderer->CreatePass(
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
            r->BindPushConstant(self, RHIShaderStageBits::Vertex | RHIShaderStageBits::Fragment, 0,
                                sizeof(GizmoPushConstant));
            r->BindShader(self, RHIShaderStageBits::Vertex, "vertMain",
                          PathsResolve("Data/Shaders/Editor/EditorGizmo.spv"));
            r->BindShader(self, RHIShaderStageBits::Fragment, "fragMain",
                          PathsResolve("Data/Shaders/Editor/EditorGizmo.spv"));
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
            if (sGizmo.vertexCount == 0u)
                return;

            GizmoPushConstant pc{};
            pc.viewProj = GEditor.camera.proj * GEditor.camera.view;
            r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Vertex | RHIShaderStageBits::Fragment, 0, pc);

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
}

void BuildLightGizmos()
{
    sGizmo.vertices.clear();
    sGizmo.vertexCount = 0u;

    if (!GEditor.showImGui || !GEditor.HasScene())
        return;

    auto lights = GEditor.Scene().GetLights();
    for (int i = 0; i < static_cast<int>(lights.size()); ++i)
    {
        FLight const& light = lights[i];
        if (light.type == FLightType::Environment)
            continue;

        float4 color = GizmoColor(i == GEditor.selectedLight);
        switch (light.type)
        {
        case FLightType::Directional: AppendDirectionalGizmo(sGizmo.vertices, light, color); break;
        case FLightType::Point:       AppendPointGizmo(sGizmo.vertices, light, color);       break;
        case FLightType::Spot:        AppendSpotGizmo(sGizmo.vertices, light, color);        break;
        case FLightType::Disk:        AppendDiskGizmo(sGizmo.vertices, light, color);        break;
        case FLightType::Rect:        AppendRectGizmo(sGizmo.vertices, light, color);          break;
        default: break;
        }
    }

    sGizmo.vertexCount = static_cast<uint32_t>(sGizmo.vertices.size());
}

} // namespace EditorGizmos
