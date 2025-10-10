#include "Camera.hpp"
namespace ModelViewer
{
    Camera::Params Camera::GetParams() const
    {
        mat4 proj = infinitePerspective(verticalFov, aspectRatio, zNear);
        mat4 view = lookAtRH(position, lookAt, up);
        proj[1][1] *= -1; // Vulkan NDC
        return {.viewProj = proj * view, .cameraPosition = position, .zNear = zNear};
    }
    Camera::CullParams Camera::GetCullParams() const
    {
        mat4 proj = perspective(verticalFov, aspectRatio, zNear, zCull);
        mat4 view = lookAtRH(position, lookAt, up);
        proj[1][1] *= -1; // Vulkan NDC
        // See also
        // - Fast Extraction of Viewing Frustum Planes from the WorldView-Projection Matrix
        // - https://github.com/zeux/niagara/blob/master/src/niagara.cpp
        mat4 projT = transpose(proj);
        // vvv Plane equations forming ax+by+cz+d=0
        float4 pLeft = projT[3] + projT[0];   // (m41 + m11, m42 + m12, m43 + m13, m44 + m14)
        float4 pTop = projT[3] + projT[1];    // (m41 + m21, m42 + m22, m43 + m23, m44 + m24)
        // ^^^
        // Note that this would be in View space (not incl. view matrix) - so origin is always on the plane
        // w=0 -> ax+by+cz=0
        // It's also easy to notice that for left/right planes y=0, for top/bottom planes x=0:
        // pLeft -> ax+cz = 0, pTop -> by+cz=0
        // Since our projection matrix is symmetric (l=-r, t=-b), so pRight, pBottom would simply be:
        // pRight -> -ax+cz=0, pBottom -> -by+cz=0
        // 4 floats would be enough for 4 planes.
        // For near and far plane - just use the view space depth.
        // vvv Normalize
        pLeft /= length(pLeft.xyz()), pTop /= length(pTop.xyz());
        return {
            .viewMatrix = view,
            .viewProj = proj * view,
            .frustum = {pLeft.x, pLeft.z, pTop.y , pTop.z,},
            .cameraPosition = position,
            .zCull = zCull,
        };
    }
}