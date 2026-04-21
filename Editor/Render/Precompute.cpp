#include "Precompute.hpp"
AliasTable::AliasTable(Span<const float> f, Allocator* alloc) :
    mProb(f.size(), alloc), mSelect(f.size(), alloc), mAlias(f.size(), alloc)
{
    // Normalize
    const uint n = f.size();
    const double sum = std::accumulate(f.begin(), f.end(), 0.0);
    for (uint i = 0; i < n; ++i)
        mProb[i] = f[i] / sum;
    // Two-halves by average
    // p_under (<1), p_over(>1) stores prob mass, therefore *n
    Vector<Pair<double, uint>> under(n, alloc), over(n, alloc);
    for (uint i = 0; i < n; ++i)
    {
        double prob = mProb[i] * n;
        if (prob >= 1.0)
            over.push_back({ prob, i });
        else
            under.push_back({ prob, i });
    }
    // Pair under & over
    while (!under.empty() && !over.empty())
    {
        const auto [p_under, i_under] = under.back();
        const auto [p_over, i_over] = over.back();
        // p_under can always be stored as prob (<1)
        mSelect[i_under] = p_under;
        // Excessive prob comes from over
        mAlias[i_under] = i_over;
        under.pop_back(), over.pop_back();
        // One bin consumes 1 mass
        double excess = p_over + p_under - 1.0;
        // Return to queue
        if (excess < 1.0)
            under.push_back({ excess, i_over });
        else
            over.push_back({ excess, i_over });
    }
    // Remaining bins
    // Don't have anything to pair with (e.g. uniform input)
    // Alias to themselves
    for (const auto [p, i] : under)
        mSelect[i] = 1.0f, mAlias[i] = i;
    for (const auto [p, i] : over)
        mSelect[i] = 1.0f, mAlias[i] = i;
}
uint AliasTable::Sample(float u, float& pdf) const
{
    const uint n = mProb.size();
    uint i = std::min(static_cast<uint>(u * n), n - 1);
    // Get another i.i.d. for i or its alias
    u = u * n - i;
    // Self or alias?
    if (u > mSelect[i])
        i = mAlias[i];
    pdf = mProb[i];
    return i;
}
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

