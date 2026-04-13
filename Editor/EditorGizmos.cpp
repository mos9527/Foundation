#include "EditorState.hpp"
#include "Scene/Mesh.hpp"
#include <Math/Decompose.hpp>

/* ==================== Helpers ==================== */

// Project a world-space point to screen-space ImVec2
static ImVec2 WorldToScreen(vec3 worldPos, mat4 const& viewProj, ImVec2 displaySize)
{
    vec4 clip = viewProj * vec4(worldPos, 1.0f);
    if (clip.w <= 0.0f)
        return {-1e4f, -1e4f}; // behind camera — off-screen sentinel
    vec3 ndc = vec3(clip) / clip.w;
    return {
        (ndc.x * 0.5f + 0.5f) * displaySize.x,
        (-ndc.y * 0.5f + 0.5f) * displaySize.y // flip Y for screen coords
    };
}

// Draw a wireframe circle in world space via ImDrawList
static void DrawWireCircle(ImDrawList* dl, vec3 center, vec3 u, vec3 v, float radius,
                           mat4 const& viewProj, ImVec2 displaySize, ImU32 color, float thickness,
                           int segments = 32)
{
    for (int i = 0; i < segments; i++)
    {
        float a0 = i * 6.2831853f / segments;
        float a1 = (i + 1) * 6.2831853f / segments;
        vec3 p0 = center + (u * cosf(a0) + v * sinf(a0)) * radius;
        vec3 p1 = center + (u * cosf(a1) + v * sinf(a1)) * radius;
        dl->AddLine(WorldToScreen(p0, viewProj, displaySize),
                    WorldToScreen(p1, viewProj, displaySize), color, thickness);
    }
}

// Draw a line between two world-space points
static void DrawWorldLine(ImDrawList* dl, vec3 a, vec3 b,
                          mat4 const& viewProj, ImVec2 displaySize, ImU32 color, float thickness)
{
    dl->AddLine(WorldToScreen(a, viewProj, displaySize),
                WorldToScreen(b, viewProj, displaySize), color, thickness);
}

/* ==================== Per-type shape overlays ==================== */

static void DrawDirectionalOverlay(FLight const& light, mat4 const& vp, ImDrawList* dl, ImVec2 ds, ImU32 col)
{
    vec3 pos = vec3(light.transform.transform);
    vec3 dir = normalize(light.transform.rotation * vec3(0, 0, -1));
    float len = 2.0f;

    // Main direction arrow
    DrawWorldLine(dl, pos, pos + dir * len, vp, ds, col, 2.0f);

    // Arrowhead: 3 lines from tip back
    float3 u, v;
    buildOrthonormalBasis(dir, u, v);
    vec3 tip = pos + dir * len;
    for (int i = 0; i < 3; i++)
    {
        float a = i * 2.0943951f; // 120° apart
        vec3 base = tip - dir * 0.3f + (u * cosf(a) + v * sinf(a)) * 0.15f;
        DrawWorldLine(dl, tip, base, vp, ds, col, 2.0f);
    }

    // Small sun-like circle at origin
    DrawWireCircle(dl, pos, u, v, 0.15f, vp, ds, col, 1.5f, 16);
}

static void DrawPointOverlay(FLight const& light, mat4 const& vp, ImDrawList* dl, ImVec2 ds, ImU32 col)
{
    vec3 pos = vec3(light.transform.transform);
    ImVec2 center = WorldToScreen(pos, vp, ds);
    if (center.x < -1e3f) return; // behind camera

    // Compute outer ring radius in pixels.
    // Only the center is projected; radius is derived analytically.
    float outerPx;
    if (light.range > 0.0f)
    {
        float dist = glm::length(pos - GCamera.position);
        float pixelsPerUnit = (ds.y * 0.5f) / (dist * tanf(GCamera.fovY * 0.5f));
        outerPx = std::max(light.range * pixelsPerUnit, 8.0f);
    }
    else
    {
        outerPx = 28.0f; // fixed screen-space size
    }

    // Concentric 2D rings — omnidirectional, no axis bias
    constexpr int kRings = 3;
    for (int i = 0; i < kRings; i++)
    {
        float t = static_cast<float>(i + 1) / kRings;
        float radius = outerPx * t;
        // Fade inner rings slightly
        ImU32 ringCol = (col & 0x00FFFFFF) | (static_cast<ImU32>((col >> 24) * (0.4f + 0.6f * t)) << 24);
        dl->AddCircle(center, radius, ringCol, 32, 1.5f);
    }

    // Small filled dot at center
    dl->AddCircleFilled(center, 3.0f, col, 12);
}

static void DrawSpotOverlay(FLight const& light, mat4 const& vp, ImDrawList* dl, ImVec2 ds, ImU32 col)
{
    vec3 pos = vec3(light.transform.transform);
    vec3 dir = normalize(light.transform.rotation * vec3(0, 0, -1));
    float coneLen = (light.range > 0.0f) ? light.range : 3.0f;
    float outerR = coneLen * tanf(light.spotOuterConeAngle);

    float3 u, v;
    buildOrthonormalBasis(dir, u, v);
    vec3 tip = pos + dir * coneLen;

    // Base circle at cone end
    DrawWireCircle(dl, tip, u, v, outerR, vp, ds, col, 1.5f, 24);

    // 4 cone edge lines from apex to base
    for (int i = 0; i < 4; i++)
    {
        float a = i * 1.5707963f; // 90° apart
        vec3 base = tip + (u * cosf(a) + v * sinf(a)) * outerR;
        DrawWorldLine(dl, pos, base, vp, ds, col, 1.5f);
    }

    // Inner cone circle (if different from outer)
    if (light.spotInnerConeAngle > 0.001f)
    {
        float innerR = coneLen * tanf(light.spotInnerConeAngle);
        DrawWireCircle(dl, tip, u, v, innerR, vp, ds, col & 0x80FFFFFF, 1.0f, 24);
    }
}

static void DrawDiskOverlay(FLight const& light, mat4 const& vp, ImDrawList* dl, ImVec2 ds, ImU32 col)
{
    vec3 pos = vec3(light.transform.transform);
    vec3 dir = normalize(light.transform.rotation * vec3(0, 0, -1));

    float3 u, v;
    buildOrthonormalBasis(dir, u, v);

    // Disk circle
    DrawWireCircle(dl, pos, u, v, light.radius, vp, ds, col, 1.5f);

    // Normal arrow
    DrawWorldLine(dl, pos, pos + dir * light.radius * 1.5f, vp, ds, col, 2.0f);
}

static void DrawRectOverlay(FLight const& light, mat4 const& vp, ImDrawList* dl, ImVec2 ds, ImU32 col)
{
    vec3 pos = vec3(light.transform.transform);
    vec3 dir = normalize(light.transform.rotation * vec3(0, 0, -1));

    float3 u, v;
    buildOrthonormalBasis(dir, u, v);

    // Rectangle corners (half-extents)
    vec3 corners[4] = {
        pos + u * light.width + v * light.height,
        pos - u * light.width + v * light.height,
        pos - u * light.width - v * light.height,
        pos + u * light.width - v * light.height,
    };
    for (int i = 0; i < 4; i++)
        DrawWorldLine(dl, corners[i], corners[(i + 1) % 4], vp, ds, col, 1.5f);

    // Normal arrow from center
    DrawWorldLine(dl, pos, pos + dir * 0.5f, vp, ds, col, 2.0f);
}

/* ==================== Public API ==================== */

void DrawLightGizmos()
{
    auto& lights = GDoc.scene.mLights;
    if (lights.empty())
        return;

    auto& io = ImGui::GetIO();
    ImVec2 displaySize = io.DisplaySize;
    ImDrawList* drawList = ImGui::GetBackgroundDrawList();
    mat4 viewProj = GCamera.proj * GCamera.view;

    // -- Shape overlays for all lights --
    for (int i = 0; i < static_cast<int>(lights.size()); i++)
    {
        bool selected = (i == GDoc.selectedLight);
        ImU32 color = selected ? IM_COL32(255, 200, 50, 255)   // gold for selected
                               : IM_COL32(255, 255, 100, 100); // dim yellow for others

        auto& light = lights[i];
        switch (light.type)
        {
        case FLightType::Directional: DrawDirectionalOverlay(light, viewProj, drawList, displaySize, color); break;
        case FLightType::Point:       DrawPointOverlay(light, viewProj, drawList, displaySize, color);       break;
        case FLightType::Spot:        DrawSpotOverlay(light, viewProj, drawList, displaySize, color);        break;
        case FLightType::Disk:        DrawDiskOverlay(light, viewProj, drawList, displaySize, color);        break;
        case FLightType::Rect:        DrawRectOverlay(light, viewProj, drawList, displaySize, color);        break;
        }
    }

    // -- ImGuizmo manipulator for the selected light --
    if (GDoc.selectedLight < 0 || GDoc.selectedLight >= static_cast<int>(lights.size()))
        return;

    auto& light = lights[GDoc.selectedLight];
    bool hasPosition = (light.type != FLightType::Directional);

    // Build model matrix from light transform (no scale — lights don't scale)
    mat4 modelMatrix = translate(mat4(1.0f), vec3(light.transform.transform))
                     * mat4_cast(light.transform.rotation);

    ImGuizmo::BeginFrame();
    ImGuizmo::SetDrawlist(drawList);
    ImGuizmo::SetRect(0, 0, io.DisplaySize.x, io.DisplaySize.y);

    // Directional: rotate only. Others: translate + rotate (never scale).
    ImGuizmo::OPERATION op = hasPosition ? GGizmo.op : ImGuizmo::ROTATE;
    if (op == ImGuizmo::SCALE)
        op = ImGuizmo::TRANSLATE; // lights don't have meaningful uniform scale

    if (ImGuizmo::Manipulate(&GCamera.view[0][0], &GCamera.proj[0][0],
                             op, GGizmo.mode, &modelMatrix[0][0]))
    {
        float3 newTranslation;
        quat newRotation;
        float3 newScale;
        Math::decompose(modelMatrix, newScale, newRotation, newTranslation);
        light.transform.transform = newTranslation;
        light.transform.rotation = newRotation;
        // Sync to GPU
        UpdateSceneLights();
        GShaderGlobals.ptAccumulatedFrames = 0;
    }
}
