#pragma once
#include <Math/Math.hpp>
namespace ModelViewer
{
    using namespace Foundation::Math;
    struct Camera
    {
        float3 position{1,1,1};
        float3 lookAt{0,0,0};
        float3 up{0, 0, 1};

        float verticalFov{radians(45.0)}; // In radians
        float aspectRatio{1};
        float zNear{1e-3};
        /**
         * @brief Construct a view-projection matrix with infinite far plane
         * @note This is only to be used with @ref gizmosDrawCameraFrustum
         */
        mat4 GetViewProjFinite(float zFar) const
        {
            mat4 p = perspective(verticalFov, aspectRatio, zNear, zFar);
            p[1][1] *= -1; // Vulkan NDC
            mat4 v = lookAtRH(position, lookAt, up);
            return p * v;
        }
        struct Params
        {
            mat4 viewProj;
            float3 cameraPosition;
            float zNear;
        };
        Params GetParams() const;
        struct CullParams
        {
            mat4 viewMatrix;
            float4 frustum; // See @ref GetCullParams
        };
        CullParams GetCullParams() const;
    };
}