#include "Gizmos.hpp"
namespace ModelViewer
{
    void drawGizmoCameraFrustum(mat4 viewProj, mat4 frustumViewProj, ImColor color, float lineThickness)
    {
        const float3 kNDC[8]{{-1, -1, 1},{-1, 1, 1},{-1, -1, -1},{-1, 1, -1},{1, -1, 1},{1, 1, 1},{1, -1, -1}, {1, 1, -1}};
        const ivec2 kEdge[12]{{0,1},{0,2},{0,4},{1,3},{1,5},{2,3},{2,6},{3,7},{4,5},{4,6},{5,7},{6,7}};
        auto [offset, region, drawList] = getGizmoDrawOffsetRegionList();
        mat4 invViewProj = inverse(frustumViewProj);
        // Frustum
        {
            ImVec2 frustum[8];
            bool cull[8]{};
            for (int i = 0; i < 8; i++)
            {
                float4 p = invViewProj * float4{ kNDC[i], 1.0f };
                p = p / p.w;
                p = viewProj * p;
                p = p / p.w;
                float2 xy = (p.xy()+float2(1,1)) * float2(0.5, 0.5);  // [-1,1] -> [0,1]
                xy.x *= region.x, xy.y *= region.y;
                if (p.z < 0 || p.z > 1)
                    cull[i] = true;
                frustum[i] = {xy.x,xy.y};
            }
            for (auto e : kEdge)
            {
                if (cull[e.x] || cull[e.y])
                    continue;
                drawList->AddLine(ImVec2(frustum[e.x].x, frustum[e.x].y) + offset,
                                  ImVec2(frustum[e.y].x, frustum[e.y].y) + offset,
                                  color, lineThickness);
            }
        }
        // Tiny hat
        {
            const float3 kHat[3]{{-1,-1.1,1},{0,-1.5,1},{1,-1.1,1}};
            ImVec2 hat[3];
            bool cull = false;
            for (int i = 0; i < 3; i++)
            {
                float4 p = invViewProj * float4{ kHat[i], 1.0f };
                p = p / p.w;
                p = viewProj * p;
                p = p / p.w;
                if (p.z < 0 || p.z > 1)
                    cull = true;
                float2 xy = (p.xy() + float2(1, 1)) * float2(0.5, 0.5);  // [-1,1] -> [0,1]
                xy.x *= region.x, xy.y *= region.y;
                hat[i] = {xy.x, xy.y};
            }
            if (!cull)
                drawList->AddTriangleFilled(hat[0] + offset,hat[1] + offset,hat[2] + offset, color);
        }
    }
} // namespace ModelViewer
