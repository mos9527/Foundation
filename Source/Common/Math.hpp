#pragma once
#include "DirectXMath.h"
#include <directxtk/SimpleMath.h>
using namespace DirectX;
// ** https://github.com/microsoft/DirectXTK/pull/298    
inline XMVECTOR XMQuaternionToPitchYawRoll(XMVECTOR Q) {
    float x = XMVectorGetX(Q), y = XMVectorGetY(Q), z = XMVectorGetZ(Q), w = XMVectorGetW(Q);
    float xx = x * x;
    float yy = y * y;
    float zz = z * z;

    float m31 = 2.f * x * z + 2.f * y * w;
    float m32 = 2.f * y * z - 2.f * x * w;
    float m33 = 1.f - 2.f * xx - 2.f * yy;

    float cy = sqrtf(m33 * m33 + m31 * m31);
    float cx = atan2f(-m32, cy);
    if (cy > 16.f * FLT_EPSILON)
    {
        float m12 = 2.f * x * y + 2.f * z * w;
        float m22 = 1.f - 2.f * xx - 2.f * zz;

        return { cx, atan2f(m31, m33),atan2f(m12, m22) };
    }
    else
    {
        float m11 = 1.f - 2.f * yy - 2.f * zz;
        float m21 = 2.f * x * y - 2.f * z * w;

        return { cx, 0.f,atan2f(-m21, m11) };
    }
}
struct AffineTransform : public SimpleMath::Matrix
{
    using SimpleMath::Matrix::Matrix; 
    AffineTransform(Vector3& translation, Quaternion& rotationQuat, Vector3& scale) : 
        SimpleMath::Matrix::Matrix(XMMatrixAffineTransformation(scale, { 0,0,0 }, rotationQuat, translation)) {};
};