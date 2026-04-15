#pragma once
#include <Core/Container.hpp>
#include <Math/Math.hpp>
using namespace Foundation;
using namespace Core;
using namespace Math;

// https://www.pbr-book.org/4ed/Sampling_Algorithms/Sampling_1D_Functions#eq:piecewise-step-integral
struct PiecewiseConstant1D
{
    Vector<float> mF, mCDF;
    float mInt;
    PiecewiseConstant1D(Span<const float> f, Allocator* alloc);

    float Int() const { return mInt; }
    // Uniform location in [0,1]
    float Sample(float u, float& pdf, uint& offset) const;
    float PDF(float sample) const {
        uint offset = clamp(static_cast<uint>(sample * mF.size()), 0u, static_cast<uint>(mF.size() - 1));
        return mInt > 0.0f ? mF[offset] / mInt : 0.0f;
    }
};
// https://www.pbr-book.org/4ed/Sampling_Algorithms/Sampling_Multidimensional_Functions#PiecewiseConstant2D
struct PiecewiseConstant2D
{
    Vector<float> mF;
    UniquePtr<PiecewiseConstant1D> mMarginal; // p(v)
    Vector<UniquePtr<PiecewiseConstant1D>> mConditional; // p(u|v)

    PiecewiseConstant2D(Span<const float> f, uint nu, uint nv, Allocator* alloc);

    uint2 Domain() const { return { mConditional[0]->mF.size(), mConditional.size() }; }

    float Int() const { return mMarginal->Int(); }
    float2 Sample(float2 u, float& pdf, uint2& offset) const;
    float PDF(float2 sample) const;
};