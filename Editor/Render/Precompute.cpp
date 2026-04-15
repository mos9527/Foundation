#include "Precompute.hpp"
PiecewiseConstant1D::PiecewiseConstant1D(Span<const float> f, Allocator* alloc):
    mF(f.begin(), f.end(), alloc), mCDF(f.size() + 1, alloc)
{
    const uint n = f.size();
    for (uint i = 1; i < mCDF.size(); ++i)
        mCDF[i] = mCDF[i - 1] + abs(f[i - 1]) / n;

    mInt = mCDF.back();
    if (mInt == 0.0f)  // Uniform if PDF=0 across the domain
        for (uint i = 1; i < mCDF.size(); ++i)
            mCDF[i] = static_cast<float>(i) / n;
    else
        for (uint i = 1; i < mCDF.size() - 1; ++i)
            mCDF[i] /= mInt;
}
float PiecewiseConstant1D::Sample(float u, float& pdf, uint& offset) const
{
    const uint n = mF.size();
    offset = Ranges::upper_bound(mCDF, u) - mCDF.begin() - 1;
    offset = clamp(offset, 0u, n - 1);
    
    const float l = mCDF[offset], h = mCDF[offset + 1];
    pdf = mInt > 0.0f ? mF[offset] / mInt : 0.0f;

    // Lerp between straddled values
    const float du = (h > l) ? (u - l) / (h - l) : 0.0f;
    return (offset + du) / n;
}
PiecewiseConstant2D::PiecewiseConstant2D(Span<const float> f, uint nu, uint nv, Allocator* alloc):
    mF(f.begin(), f.end(), alloc), mConditional(alloc)
{
    CHECK(f.size() == nu * nv);
    for (uint v = 0; v < nv; v++)
        mConditional.emplace_back(ConstructUnique<PiecewiseConstant1D>(alloc, f.subspan(v * nu, nu), alloc));
    Vector<float> marginal(nv, alloc);
    for (uint v = 0; v < nv; v++)
        marginal[v] = mConditional[v]->mInt;
    mMarginal = ConstructUnique<PiecewiseConstant1D>(alloc, marginal, alloc);
}
float2 PiecewiseConstant2D::Sample(float2 u, float& pdf, uint2& offset) const
{
    float2 pdfs, res;
    res.y = mMarginal->Sample(u.y, pdfs.y, offset.y);
    res.x = mConditional[offset.y]->Sample(u.x, pdfs.x, offset.x);
    pdf = pdfs.x * pdfs.y; // p(u,v) = p(v) * p(u|v)
    return res;
}
float PiecewiseConstant2D::PDF(float2 sample) const
{
    const uint2 domain = Domain();
    uint2 offset = { sample.x * domain.x, sample.y * domain.y };
    offset = clamp(offset, uint2(0), domain - uint2(1));
    return mMarginal->Int() > 0.0f ? mConditional[offset.y]->mF[offset.x] / mMarginal->Int() : 0.0f;
}

