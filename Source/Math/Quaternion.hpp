#pragma once
#include "Math.hpp"
namespace Foundation::Math
{
    // Packs quaternion to [UNORM XYZ, 2-bit max index]
    inline float4 packQuaternionXYZPositionBit(quat const& q)
    {
        float4 Q(q.x, q.y, q.z, q.w);
        float4 absQ(abs(Q.x), abs(Q.y), abs(Q.z), abs(Q.w));
        float absMax = max(max(absQ.x, absQ.y), max(absQ.z, absQ.w));
        uint maxIndex = 0;
        if (absQ[0] == absMax)
            maxIndex = 0;
        if (absQ[1] == absMax)
            maxIndex = 1;
        if (absQ[2] == absMax)
            maxIndex = 2;
        if (absQ[3] == absMax)
            maxIndex = 3;
        if (Q[maxIndex] < 0) // ensure positive
            Q = -Q;
        float3 packed;
        if (maxIndex == 0)
            packed = Q.yzw();
        if (maxIndex == 1)
            packed = Q.xzw();
        if (maxIndex == 2)
            packed = Q.xyw();
        else /* maxIndex == 3 */
            packed = Q.xyz();
        packed *= sqrt(2.0f); // e.g. (1,0,0,1), max bounds
        packed = packed * 0.5f + 0.5f; // [-1,1] -> [0,1]
        return float4(packed, maxIndex / 3.0f);
    }

    // Unpacks quaternion from [UNORM XYZ, 2-bit max index] to quat
    inline quat unpackQuaternionXYZPositionBit(float4 const& packed)
    {
        uint maxIndex = packed.w * 3.0f;
        float3 p = packed.xyz() * 2.0f - 1.0f; // [0,1] -> [-1,1]
        p /= sqrt(2.0f);
        float4 Q;
        float maxValue = sqrt(max(.0f, 1 - p.x * p.x - p.y * p.y - p.z * p.z));
        if (maxIndex == 0)
            Q = float4(maxValue, p.xyz);
        else if (maxIndex == 1)
            Q = float4(p.x, maxValue, p.yz);
        else if (maxIndex == 2)
            Q = float4(p.xy, maxValue, p.z);
        else /* maxIndex == 3 */
            Q = float4(p.xyz, maxValue);
        return quat(Q.x, Q.y, Q.z, Q.w);
    }
} // namespace Foundation::Math
