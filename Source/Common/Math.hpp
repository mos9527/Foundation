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

enum class RotationType {
    PitchYawRoll,
    Quaternion,
    Unknown
};
struct Rotation {
protected:
    // Rotation
    // Rotation quaterion in XYZW
    XMVECTOR RotationQuaternion = { 0,0,0,1 };
    // Euler Rotation
    XMVECTOR RotationPitchYawRoll = { 0,0,0 };
    // Direction vector with 0-rotation vector being (0,0,1)
    XMVECTOR DirectionVector = { 0,0,1 };
public:
    /* Ctor */
    Rotation(RotationType mode = RotationType::Quaternion) : RotationMode(mode) {};
    // Initialize the Rotation w/o specifing a rotation mode
    // The mode is then implictly specified by the math operator's LHS
    // Useful if you're doing something like:
    // Rot_Camera += { delta_roll, delta_pitch, 0 };
    Rotation(XMVECTOR R) : RotationMode(RotationType::Unknown) {
        RotationQuaternion = RotationPitchYawRoll = R;
    }
    // Initialize the rotation with fully qualified mode and R vector
    Rotation(RotationType mode, XMVECTOR R) : RotationMode(mode) {
        this->operator=(R);
    }
    /* Setters / Getters */
    void SetRotationPitchYawRoll() {
        // Acutally, eulers are in PitchYawRoll! But rotation is done in yaw-pitch-roll order.
        RotationQuaternion = XMQuaternionRotationRollPitchYawFromVector(RotationPitchYawRoll);
        DirectionVector = XMVector3Rotate({ 0,0,1 }, GetRotationQuaternion());
    }
    void SetRotationPitchYawRoll(XMVECTOR eulers) {
        RotationPitchYawRoll = eulers;
        SetRotationPitchYawRoll();
    }
    void SetRotationQuaternion() {
        RotationPitchYawRoll = XMQuaternionToPitchYawRoll(RotationQuaternion);
        DirectionVector = XMVector3Rotate({ 0,0,1 }, GetRotationQuaternion());
    }
    void SetRotationQuaternion(XMVECTOR quaternion) {
        RotationQuaternion = quaternion;
        SetRotationQuaternion();
    }
    XMVECTOR GetRotationQuaternion() const { return RotationQuaternion; }
    XMVECTOR GetRotationPitchYawRoll() const { return RotationPitchYawRoll; }
    // With 0-rotation vector being (0,0,1)
    XMVECTOR GetDirectionVector() const { return DirectionVector; }

    void AddRotationQuaternion(XMVECTOR Q) { SetRotationQuaternion(XMQuaternionMultiply(RotationQuaternion, Q)); }
    void AddRotationPitchYawRoll(XMVECTOR E) { SetRotationPitchYawRoll(RotationPitchYawRoll + E); }

    /* Operators */
    // Rotation Mode. Useful for operators.
    RotationType RotationMode = RotationType::Unknown;
    operator XMVECTOR() {
        if (RotationMode == RotationType::Quaternion)
            return RotationQuaternion;
        else if (RotationMode == RotationType::PitchYawRoll)
            return RotationPitchYawRoll;
        else return RotationQuaternion;
    }
    void operator=(Rotation lhs) {
        if (RotationMode == RotationType::Quaternion)
            SetRotationQuaternion(lhs.RotationQuaternion);
        else if (RotationMode == RotationType::PitchYawRoll)
            SetRotationPitchYawRoll(lhs.RotationPitchYawRoll);
    }
    Rotation operator +=(Rotation lhs) {
        if (RotationMode == RotationType::Quaternion)
            AddRotationQuaternion(lhs.RotationQuaternion);
        else if (RotationMode == RotationType::PitchYawRoll)
            AddRotationPitchYawRoll(lhs.RotationPitchYawRoll);
        return *this;
    }
    Rotation operator +(Rotation lhs) {
        Rotation ret = *this;
        ret += lhs;
        return ret;
    }
};

struct AffineTransform
{
    SimpleMath::Vector3 translation;
    Rotation rotation;
    SimpleMath::Vector3 scale;

    void SetTransforms(SimpleMath::Matrix transform) {        
        XMVECTOR Scale, RotationQuaternion, Translation;
        XMMatrixDecompose(&Scale, &RotationQuaternion, &Translation, transform);
        scale = Scale; 
        rotation.SetRotationQuaternion(RotationQuaternion);
        translation = Translation;
    }
    SimpleMath::Matrix GetTransforms() const {
        return XMMatrixAffineTransformation(scale, { 0,0,0 }, rotation.GetRotationQuaternion(), translation);
    }
    AffineTransform() = default;    
    AffineTransform(SimpleMath::Matrix transform) { SetTransforms(transform); }
    inline operator SimpleMath::Matrix() const { return GetTransforms(); }
    friend AffineTransform operator* (const AffineTransform& lhs, const AffineTransform& rhs) {        
        AffineTransform new_transform = lhs;
        new_transform.translation += rhs.translation;
        new_transform.rotation += rhs.rotation;
        new_transform.scale *= rhs.scale;
        return new_transform;
    }
};