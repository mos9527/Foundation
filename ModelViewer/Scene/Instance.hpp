#pragma once
#include <Math/Math.hpp>
namespace ModelViewer
{
    using namespace Foundation::Math;
    /**
     * 4-byte aligned. Allocated in the Instance buffer in the @ref GPUScene
     */
    struct Instance
    {
        float3 t; // Translation
        quat q; // Rotation Quat (xyzw)
        float3 s; // Scale
        // @ref MeshAllocation::selfRawOffset + 1
        // 0 reserved for no mesh
        uint32_t meshAllocationRawOffsetPP = 0;
    };
}