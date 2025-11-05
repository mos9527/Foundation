#pragma once
#include <Math/Math.hpp>
namespace ModelViewer
{
    using namespace Foundation::Math;
    struct Camera
    {
        float3 position{1,1,1};
        quat orientation{0,0,0,1}; // wxyz

        float verticalFov{radians(45.0)}; // In radians
        float aspectRatio{1};
        float zNear{1e-3};

        struct Params
        {
            mat4 viewProj;
            float3 cameraPosition;
            float zNear;
        };
        Params GetParams() const;

        float zCull{1e2}; // Max draw distance before culling
        struct CullParams
        {
            mat4 viewMatrix;
            mat4 viewProj;
            float4 frustumACBC; // See @ref GetCullParams
            float3 cameraPosition;
            float zCull;
        };
        CullParams GetCullParams() const;

        void OnImGui();
    };
}