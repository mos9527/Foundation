// Visualizes CIE chromaticity data and common display gamut primaries.
// Includes an interactive view over spectral locus and XYZ matching curves.
#include <RenderCore/ImmediateContext.hpp>
#include <RenderUtils/CSDebugText.hpp>
#include <algorithm>
#include <cstddef>
#include <cstring>
#include "Examples.hpp"

#include <Renderer/Tables.hpp>
using namespace RenderUtils;

struct CIEPrimaries
{
    const char* name;
    float2 R, G, B, W;
};
constexpr float2 D65 = {0.3127f, 0.3290f};
constexpr uint32_t kReferencePrimariesOverlay = ~0u;
constexpr CIEPrimaries kCIEPrimaries[] = {
    {"sRGB / BT.709", {0.640f, 0.330f}, {0.300f, 0.600f}, {0.150f, 0.060f}, D65},
    {"BT.2020", {0.708f, 0.292f}, {0.170f, 0.797f}, {0.131f, 0.046f}, D65},
    {"Display P3", {0.680f, 0.320f}, {0.265f, 0.690f}, {0.150f, 0.060f}, D65},
    {"Adobe RGB (1998)", {0.640f, 0.330f}, {0.210f, 0.710f}, {0.150f, 0.060f}, D65},
};

struct ViewPushConstant
{
    float centerX{0.5f};
    float centerY{0.5f};
    float rangeX{1.0f};
    float rangeY{1.0f};
    uint32_t mode{0};
};

struct CIEVertex
{
    float2 position{};
    float4 color{};
};

struct MeshRange
{
    uint32_t firstVertex{};
    uint32_t vertexCount{};
};

enum class CIERenderMode : uint32_t
{
    ChromaticityXY,
    XYZCurves,
    Count,
};

constexpr uint32_t ModeIndex(CIERenderMode mode) { return static_cast<uint32_t>(mode); }

static float2 SampleCIELocus(uint32_t i)
{
    double X = kCIEMatchingCurveX[i];
    double Y = kCIEMatchingCurveY[i];
    double Z = kCIEMatchingCurveZ[i];
    double denom = X + Y + Z;
    return float2{static_cast<float>(X / denom), static_cast<float>(Y / denom)};
}

static void AppendTriangle(Vector<CIEVertex>& vertices, float2 p0, float2 p1, float2 p2, float4 color)
{
    vertices.push_back({.position = p0, .color = color});
    vertices.push_back({.position = p1, .color = color});
    vertices.push_back({.position = p2, .color = color});
}

template <typename FPointAt>
static MeshRange AppendPolylineAsTriangles(Vector<CIEVertex>& vertices, uint32_t pointCount, FPointAt&& pointAt,
                                           float4 color, float width, bool closed)
{
    MeshRange range{.firstVertex = static_cast<uint32_t>(vertices.size())};
    if (pointCount < 2)
        return range;

    uint32_t segmentCount = closed ? pointCount : pointCount - 1;
    float halfWidth = width * 0.5f;
    for (uint32_t i = 0; i < segmentCount; ++i)
    {
        float2 p0 = pointAt(i);
        float2 p1 = pointAt((i + 1) % pointCount);
        float2 delta = p1 - p0;
        float len = length(delta);
        if (len <= 1e-6f)
            continue;

        float2 n = float2{-delta.y, delta.x} * (halfWidth / len);
        vertices.push_back({.position = p0 - n, .color = color});
        vertices.push_back({.position = p0 + n, .color = color});
        vertices.push_back({.position = p1 + n, .color = color});
        vertices.push_back({.position = p0 - n, .color = color});
        vertices.push_back({.position = p1 + n, .color = color});
        vertices.push_back({.position = p1 - n, .color = color});
    }

    range.vertexCount = static_cast<uint32_t>(vertices.size()) - range.firstVertex;
    return range;
}

static MeshRange AppendCIEChromaticityFill(Vector<CIEVertex>& vertices, float luminance)
{
    MeshRange range{.firstVertex = static_cast<uint32_t>(vertices.size())};
    float2 center = D65;
    float4 color{0.0f, 0.0f, 0.0f, luminance};
    for (uint32_t i = 0; i < kCIESamples; ++i)
    {
        AppendTriangle(vertices, center, SampleCIELocus(i), SampleCIELocus((i + 1) % kCIESamples), color);
    }
    range.vertexCount = static_cast<uint32_t>(vertices.size()) - range.firstVertex;
    return range;
}

static MeshRange AppendCIEPrimariesFill(Vector<CIEVertex>& vertices, uint32_t primariesIndex)
{
    MeshRange range{.firstVertex = static_cast<uint32_t>(vertices.size())};
    const CIEPrimaries& primaries = kCIEPrimaries[primariesIndex];
    AppendTriangle(vertices, primaries.R, primaries.G, primaries.B, float4{0.0f, 0.0f, 0.0f, 1.0f});
    range.vertexCount = static_cast<uint32_t>(vertices.size()) - range.firstVertex;
    return range;
}

static float MaxCIEMatchingValue()
{
    double maxValue = 0.0;
    for (uint32_t i = 0; i < kCIESamples; ++i)
    {
        maxValue = std::max(maxValue, kCIEMatchingCurveX[i]);
        maxValue = std::max(maxValue, kCIEMatchingCurveY[i]);
        maxValue = std::max(maxValue, kCIEMatchingCurveZ[i]);
    }
    return static_cast<float>(maxValue);
}

static MeshRange AppendXYZCurves(Vector<CIEVertex>& vertices)
{
    MeshRange range{.firstVertex = static_cast<uint32_t>(vertices.size())};
    float maxValue = MaxCIEMatchingValue();
    auto curveX = [=](uint32_t i, const double* curve)
    {
        return float2{static_cast<float>(i) / static_cast<float>(kCIESamples - 1),
                      static_cast<float>(curve[i]) / maxValue};
    };
    constexpr float kLineWidth = 0.006f;
    AppendPolylineAsTriangles(
        vertices, kCIESamples, [&](uint32_t i) { return curveX(i, kCIEMatchingCurveX); },
        float4{1.0f, 0.2f, 0.2f, 1.0f}, kLineWidth, false);
    AppendPolylineAsTriangles(
        vertices, kCIESamples, [&](uint32_t i) { return curveX(i, kCIEMatchingCurveY); },
        float4{0.2f, 1.0f, 0.2f, 1.0f}, kLineWidth, false);
    AppendPolylineAsTriangles(
        vertices, kCIESamples, [&](uint32_t i) { return curveX(i, kCIEMatchingCurveZ); },
        float4{0.3f, 0.5f, 1.0f, 1.0f}, kLineWidth, false);
    range.vertexCount = static_cast<uint32_t>(vertices.size()) - range.firstVertex;
    return range;
}
int main(int argc, char** argv)
{
    SDL_Window* window = SDL_CreateWindow(FOUNDATION_APPLICATION_TITLE("CIE Chromaticity Example"), 1024, 768,
                                          Examples_SDLWindowFlagsVulkan);
    auto ctx = Examples_InitVulkan(window, argc, argv,
                                   {
                                       .threadCount = 0 /* ST recording */
                                   });

    Vector<CIEVertex> cieMesh(GLOBAL_ALLOC);
    cieMesh.reserve(kCIESamples * 3 + static_cast<uint32_t>(std::size(kCIEPrimaries)) * 3 + kCIESamples * 6 * 3);
    MeshRange chromaticityRange = AppendCIEChromaticityFill(cieMesh, 0.1f);
    MeshRange referenceChromaticityRange = AppendCIEChromaticityFill(cieMesh, 1.0f);
    MeshRange primariesFillRanges[std::size(kCIEPrimaries)]{};
    for (uint32_t i = 0; i < std::size(kCIEPrimaries); ++i)
    {
        primariesFillRanges[i] = AppendCIEPrimariesFill(cieMesh, i);
    }
    MeshRange xyzCurvesRange = AppendXYZCurves(cieMesh);
    size_t cieMeshBytes = cieMesh.size() * sizeof(CIEVertex);
    auto cieMeshBuffer =
        ImmediateCreateBuffer(ctx.device.Get(),
                              {.resource = {.heap = RHIDeviceHeapType::Local, .shared = false},
                               .usage = RHIBufferUsageBits::VertexBuffer | RHIBufferUsageBits::TransferDestination,
                               .size = cieMeshBytes},
                              cieMesh.data(), cieMeshBytes);
    ExampleInputState input{};
    CIERenderMode activeMode = CIERenderMode::ChromaticityXY;
    uint32_t activePrimariesOverlay = kReferencePrimariesOverlay;
    ViewPushConstant viewPC[ModeIndex(CIERenderMode::Count)]{
        ViewPushConstant{0.5f, 0.5f, 1.0f, 1.0f, ModeIndex(CIERenderMode::ChromaticityXY)},
        ViewPushConstant{0.5f, 0.5f, 1.0f, 1.0f, ModeIndex(CIERenderMode::XYZCurves)},
    };
    ctx.renderer->BeginSetup();
    auto cieMeshHandle = ctx.renderer->CreateResource("CIE Chromaticity Mesh", cieMeshBuffer.Release().Get());
    ctx.renderer->CreatePass(
        "CIE Chromaticity", RHIDeviceQueueType::Graphics, 0u,
        [=](PassHandle self, Renderer* r)
        {
            r->BindBackbufferRTV(self);
            r->BindBufferShaderRead(self, cieMeshHandle, RHIPipelineStageBits::VertexShader);
            r->BindVertexInput(self,
                               {.bindings = {{{sizeof(CIEVertex), false}}},
                                .attributes = {{
                                    {.location = 0,
                                     .offset = offsetof(CIEVertex, position),
                                     .format = RHIResourceFormat::R32G32SignedFloat},
                                    {.location = 1,
                                     .offset = offsetof(CIEVertex, color),
                                     .format = RHIResourceFormat::R32G32B32A32SignedFloat},
                                }}});
            r->BindPushConstant(self, RHIShaderStageBits::Vertex | RHIShaderStageBits::Fragment, 0,
                                sizeof(ViewPushConstant));
            r->BindShader(self, RHIShaderStageBits::Vertex, "vertMain",
                          Foundation::Core::PathsResolve("Data/Shaders/CIEChromacity.spv"));
            r->BindShader(self, RHIShaderStageBits::Fragment, "fragMain",
                          Foundation::Core::PathsResolve("Data/Shaders/CIEChromacity.spv"));
            r->PassSetRasterizerFlags(self, {.cullMode = RHIPipelineState::PipelineStateDesc::Rasterizer::CullNone},
                                      {.depthTest = false, .depthWrite = false});
        },
        [=, &activeMode, &activePrimariesOverlay, &viewPC](PassHandle self, Renderer* r, RHICommandList* cmd)
        {
            auto const& imgWh = r->GetSwapchainExtent();
            auto* mesh = r->DerefResource(cieMeshHandle).Get<RHIBuffer*>();
            uint32_t modeIdx = ModeIndex(activeMode);
            MeshRange range = activeMode == CIERenderMode::XYZCurves   ? xyzCurvesRange
                : activePrimariesOverlay == kReferencePrimariesOverlay ? referenceChromaticityRange
                                                                       : chromaticityRange;
            r->CmdSetPipeline(self, cmd);
            r->CmdBeginGraphics(self, cmd, imgWh, {{{RHIAttachmentLoadOp::Clear, {0.0f, 0.0f, 0.0f, 1.0f}}}});
            r->CmdSetPushConstant(self, cmd, RHIShaderStageBits::Vertex | RHIShaderStageBits::Fragment, 0,
                                  viewPC[modeIdx]);
            cmd->SetViewport(0, 0, imgWh.x, imgWh.y)
                .SetScissor(0, 0, imgWh.x, imgWh.y)
                .BindVertexBuffer(0, {{mesh}}, {{range.firstVertex * sizeof(CIEVertex)}})
                .Draw(range.vertexCount);
            if (activeMode == CIERenderMode::ChromaticityXY && activePrimariesOverlay != kReferencePrimariesOverlay)
            {
                MeshRange overlay = primariesFillRanges[activePrimariesOverlay];
                cmd->BindVertexBuffer(0, {{mesh}}, {{overlay.firstVertex * sizeof(CIEVertex)}})
                    .Draw(overlay.vertexCount);
            }
            cmd->EndGraphics();
        });
    createCSDebugTextPassBackBuffer(ctx.renderer.get(), "Debug Text", Examples_HudLines(input));
    ctx.renderer->EndSetup();

    ExampleFpsCounter fps;
    while (true)
    {
        Examples_BeginFrameInput(input);
        if (Examples_PollEvents(window, ctx, input))
            break;

        if (input.KeyPressed(SDLK_SPACE) || Examples_Button(input, "Mode"))
        {
            activeMode =
                activeMode == CIERenderMode::ChromaticityXY ? CIERenderMode::XYZCurves : CIERenderMode::ChromaticityXY;
        }
        Examples_SameLine(input);
        if (input.KeyPressed(SDLK_TAB) || Examples_Button(input, "Overlay"))
        {
            activePrimariesOverlay = activePrimariesOverlay == kReferencePrimariesOverlay
                ? 0u
                : (activePrimariesOverlay + 1u) % (static_cast<uint32_t>(std::size(kCIEPrimaries)) + 1u);
            if (activePrimariesOverlay == std::size(kCIEPrimaries))
                activePrimariesOverlay = kReferencePrimariesOverlay;
        }
        Examples_SameLine(input);
        uint32_t modeIdx = ModeIndex(activeMode);
        ViewPushConstant& activeView = viewPC[modeIdx];
        if (input.KeyPressed(SDLK_R) || Examples_Button(input, "Reset"))
            activeView = ViewPushConstant{0.5f, 0.5f, 1.0f, 1.0f, modeIdx};

        if (glm::dot(input.panDelta, input.panDelta) > 1e-6f)
        {
            auto extent = ctx.renderer->GetSwapchainExtent();
            activeView.centerX -= input.panDelta.x * activeView.rangeX / static_cast<float>(extent.x);
            activeView.centerY += input.panDelta.y * activeView.rangeY / static_cast<float>(extent.y);
        }
        if (std::abs(input.zoomDelta) > 1e-6f)
        {
            float factor = std::clamp(1.0f - input.zoomDelta * 0.1f, 0.25f, 4.0f);
            activeView.rangeX = std::clamp(activeView.rangeX * factor, 0.001f, 8.0f);
            activeView.rangeY = std::clamp(activeView.rangeY * factor, 0.001f, 9.0f);
        }

        Examples_Text(input,
                      activeMode == CIERenderMode::ChromaticityXY ? "CIE xy chromaticity -> BT.709/sRGB"
                                                                  : "CIE 1931 XYZ matching curves");
        Examples_Text(input, Format( {}", fps.Update()));
        Examples_Text(input,
                      activeMode == CIERenderMode::ChromaticityXY
                          ? activePrimariesOverlay == kReferencePrimariesOverlay
                              ? "Overlay: reference horseshoe"
                              : FormaFormat({}", kCIEPrimaries[activePrimariesOverlay].name)
                          : "CPU line raster: X(red), Y(green), Z(blue), normalized");
        Examples_Text(input,
                      Format("ceFormat(, {:.4f}) range=({:.4f}, {:.4f})", activeView.centerX,
                                  activeView.centerY, activeView.rangeX, activeView.rangeY));
        Examples_NewFrame(window, ctx);
    }

    Examples_DestroyVulkan(window, ctx);
    return 0;
}
