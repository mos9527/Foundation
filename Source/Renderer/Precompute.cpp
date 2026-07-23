#include "Precompute.hpp"
#include <Core/JobSystem.hpp>
#include <cmath>
#include <numbers>
PiecewiseConstant1D::PiecewiseConstant1D(Span<const float> f, Allocator* alloc) :
    mF(f.begin(), f.end(), alloc), mCDF(f.size(), alloc)
{
    const uint n = f.size();
    mCDF[0] = abs(f[0]) / n;
    for (uint i = 1; i < n; ++i)
        mCDF[i] = mCDF[i - 1] + abs(f[i]) / n;

    mInt = mCDF.back();
    if (mInt == 0.0f) // Uniform if PDF=0 across the domain
        for (uint i = 0; i < n; ++i)
            mCDF[i] = static_cast<float>(i + 1) / n;
    else
    {
        for (uint i = 0; i < n - 1; ++i)
            mCDF[i] /= mInt;
        mCDF.back() = 1.0f;
    }
}
float PiecewiseConstant1D::Sample(float u, float& pdf, uint& offset) const
{
    const uint n = mF.size();
    offset = Ranges::upper_bound(mCDF, u) - mCDF.begin();
    offset = clamp(offset, 0u, n - 1);

    const float l = offset == 0 ? 0.0f : mCDF[offset - 1];
    const float h = mCDF[offset];
    pdf = mInt > 0.0f ? mF[offset] / mInt : 0.0f;

    // Lerp between straddled values
    const float du = (h > l) ? (u - l) / (h - l) : 0.0f;
    return (offset + du) / n;
}
PiecewiseConstant2D::PiecewiseConstant2D(Span<const float> f, uint nu, uint nv, Allocator* alloc) :
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
    uint2 offset = {sample.x * domain.x, sample.y * domain.y};
    offset = clamp(offset, uint2(0), domain - uint2(1));
    return mMarginal->Int() > 0.0f ? mConditional[offset.y]->mF[offset.x] / mMarginal->Int() : 0.0f;
}
namespace
{
    constexpr float kReflectionRoughestMip = 1.0f;
    constexpr float kReflectionRoughnessMipScale = 1.2f;
    constexpr float kPi = std::numbers::pi_v<float>;
    constexpr float kInvPi = 1.0f / kPi;

    void SHEvalBasis9(float3 d, float y[9])
    {
        float x = d.x, yv = d.y, z = d.z;
        float x2 = x * x, y2 = yv * yv, z2 = z * z;
        y[0] = 0.2820947917738781f;
        y[1] = 0.4886025119029199f * yv;
        y[2] = 0.4886025119029199f * z;
        y[3] = 0.4886025119029199f * x;
        y[4] = 1.0925484305920790f * x * yv;
        y[5] = 1.0925484305920790f * yv * z;
        y[6] = 0.3153915652525200f * (3.0f * z2 - 1.0f);
        y[7] = 1.0925484305920790f * x * z;
        y[8] = 0.5462742152960395f * (x2 - y2);
    }
    constexpr float kSHConvScale[9] = {kPi,        2.0f * kPi / 3.0f, 2.0f * kPi / 3.0f, 2.0f * kPi / 3.0f, kPi / 4.0f,
                                       kPi / 4.0f, kPi / 4.0f,        kPi / 4.0f,        kPi / 4.0f};
    float2 Hammersley2D(uint32_t i, uint32_t n)
    {
        uint32_t bits = i;
        bits = (bits << 16u) | (bits >> 16u);
        bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
        bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
        bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
        bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
        float v = static_cast<float>(bits) * 2.3283064365386963e-10f; // 1/2^32
        return {static_cast<float>(i) / static_cast<float>(n), v};
    }
    float3 CosineSampleHemisphere(float2 e)
    {
        float r = std::sqrt(e.x);
        float phi = 2.0f * kPi * e.y;
        return {r * std::cos(phi), r * std::sin(phi), std::sqrt(std::max(0.0f, 1.0f - r * r))};
    }
    float3 ImportanceSampleGGX(float2 e, float a2)
    {
        float phi = 2.0f * kPi * e.x;
        float cosTheta2 = (1.0f - e.y) / (1.0f + (a2 - 1.0f) * e.y);
        float cosTheta = std::sqrt(std::clamp(cosTheta2, 0.0f, 1.0f));
        float sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta2));
        return {sinTheta * std::cos(phi), sinTheta * std::sin(phi), cosTheta};
    }
} // namespace
float ReflectionRoughnessFromMip(uint32_t mip, uint32_t numMips)
{
    float levelFrom1x1 = static_cast<float>(numMips - 1 - mip);
    return std::exp2((kReflectionRoughestMip - levelFrom1x1) / kReflectionRoughnessMipScale);
}
float ReflectionMipFromRoughness(float roughness, uint32_t numMips)
{
    float levelFrom1x1 = kReflectionRoughestMip - kReflectionRoughnessMipScale * std::log2(std::max(roughness, 0.001f));
    return static_cast<float>(numMips - 1) - levelFrom1x1;
}
void PrefilterEnvmapSH9(const FTexture& source, Span<float3> sh9, JobSystem* jobs, Allocator* alloc)
{
    CHECK(jobs != nullptr);
    CHECK(alloc != nullptr);
    CHECK(sh9.size() == 9);
    CHECK_MSG(source.GetFormat() == RHIResourceFormat::R32G32B32A32SignedFloat,
              "Envmap prefilter expects RGBA32F. Got {}", source.GetFormat());
    const uint32_t width = source.GetWidth();
    const uint32_t height = source.GetHeight();
    const float4* pixels = reinterpret_cast<const float4*>(source.GetSubresource(0, 0).data());
    using SHRow = Array<float3, 9>;
    Vector<SHRow> rows(height, SHRow{}, alloc);
    size_t const grain = std::max<size_t>(1u, height / std::max<size_t>(jobs->GetMaxConcurrency() * 4u, 1u));
    jobs->Wait(jobs->ParallelFor(
        ExecutionPolicy::Par, "EnvSH", height, grain,
        [&](size_t begin, size_t end, JobContext&)
        {
            for (size_t y = begin; y < end; ++y)
            {
                float v = (static_cast<float>(y) + 0.5f) / height;
                float theta = v * kPi;
                float sinTheta = std::sin(theta);
                float dw = (kPi / height) * (2.0f * kPi / width) * sinTheta;
                for (uint32_t x = 0; x < width; ++x)
                {
                    float u = (x + 0.5f) / width;
                    float3 dir = EquirectUVToDirection({u, v});
                    float yb[9];
                    SHEvalBasis9(dir, yb);
                    float4 p = pixels[y * width + x];
                    float3 rad = {p.x, p.y, p.z};
                    for (int b = 0; b < 9; ++b)
                        rows[y][b] += rad * yb[b] * dw;
                }
            }
        }));
    for (float3& coeff : sh9)
        coeff = float3(0.0f);
    for (SHRow const& row : rows)
        for (int b = 0; b < 9; ++b)
            sh9[b] += row[b];
    for (int b = 0; b < 9; ++b)
        sh9[b] *= kSHConvScale[b] * kInvPi;
}
FTexture PrefilterEnvmapSpecular(const FTexture& source, JobSystem* jobs, Allocator* alloc)
{
    CHECK(jobs != nullptr);
    CHECK_MSG(source.GetFormat() == RHIResourceFormat::R32G32B32A32SignedFloat,
              "Envmap prefilter expects RGBA32F. Got {}", source.GetFormat());
    const uint32_t width = source.GetWidth();
    const uint32_t height = source.GetHeight();
    FTexture pyramid(alloc);
    pyramid.Initialize(source.GetFormat(), source.GetDimension(), width, height, 1, 1, 1);
    pyramid.bytes.assign(source.bytes.begin(), source.bytes.end());
    pyramid.GenerateMips();
    const uint32_t numMips = pyramid.GetNumMips();

    Vector<uint32_t> mipW(numMips, alloc), mipH(numMips, alloc);
    Vector<const float4*> mipPtrs(numMips, alloc);
    for (uint32_t m = 0; m < numMips; ++m)
    {
        mipW[m] = std::max(1u, width >> m);
        mipH[m] = std::max(1u, height >> m);
        auto sub = pyramid.GetSubresource(m, 0);
        mipPtrs[m] = reinterpret_cast<const float4*>(sub.data());
    }

    FTexture result(alloc);
    result.Initialize(source.GetFormat(), source.GetDimension(), width, height, 1, numMips, 1);
    result.bytes.resize(result.GetSize());
    std::memcpy(result.GetSubresource(0, 0).data(), source.GetSubresource(0, 0).data(),
                source.GetSubresourceSize(0, 0));

    for (uint32_t mip = 1; mip < numMips; ++mip)
    {
        const uint32_t mipWidth = std::max(1u, width >> mip);
        const uint32_t mipHeight = std::max(1u, height >> mip);
        const float roughness = ReflectionRoughnessFromMip(mip, numMips);
        const float a2 = std::pow(roughness, 4.0f);
        const uint32_t numSamples = roughness < 0.1f ? 32u : 64u;
        const float texelSolidAngle = (4.0f * kPi) / static_cast<float>(6u * mipWidth * mipHeight) * 2.0f;
        float4* dst = reinterpret_cast<float4*>(result.GetSubresource(mip, 0).data());

        size_t const grain =
            std::max<size_t>(1u, mipHeight / std::max<size_t>(jobs->GetMaxConcurrency() * 4u, 1u));
        jobs->Wait(jobs->ParallelFor(
            ExecutionPolicy::Par, "EnvSpecular", mipHeight, grain,
            [&](size_t begin, size_t end, JobContext&)
            {
                for (size_t y = begin; y < end; ++y)
                    for (uint32_t x = 0; x < mipWidth; ++x)
                    {
                        float u = (x + 0.5f) / mipWidth;
                        float v = (static_cast<float>(y) + 0.5f) / mipHeight;
                        float3 n = EquirectUVToDirection({u, v});
                        float3 up = std::abs(n.y) < 0.999f ? float3(0, 1, 0) : float3(1, 0, 0);
                        float3 t = normalize(cross(up, n));
                        float3 b = cross(n, t);
                        mat3 toWorld(t, b, n);

                        float4 filtered(0.0f);
                        float weight = 0.0f;
                        auto AccumulateSample = [&](float3 l, float pdf)
                        {
                            float NoL = l.z;
                            if (NoL <= 0.0f || pdf <= 0.0f)
                                return;
                            float sampleSolidAngle = 1.0f / (numSamples * pdf);
                            float srcMip = 0.5f * std::log2(sampleSolidAngle / texelSolidAngle);
                            float3 w = normalize(toWorld * l);
                            float2 uv = EquirectDirectionToUV(w);
                            float4 s = SampleF32Trilinear(
                                mipPtrs.data(), mipW.data(), mipH.data(), numMips, uv.x, uv.y, srcMip);
                            filtered += s * NoL;
                            weight += NoL;
                        };

                        if (roughness > 0.99f)
                        {
                            for (uint32_t i = 0; i < numSamples; ++i)
                            {
                                float2 e = Hammersley2D(i, numSamples);
                                float3 l = CosineSampleHemisphere(e);
                                AccumulateSample(l, l.z * kInvPi);
                            }
                        }
                        else
                        {
                            for (uint32_t i = 0; i < numSamples; ++i)
                            {
                                float2 e = Hammersley2D(i, numSamples);
                                e.y *= 0.995f;
                                float3 h = ImportanceSampleGGX(e, a2);
                                float3 l = 2.0f * h.z * h - float3(0, 0, 1);
                                float NoH = h.z;
                                float denom = std::pow(1.0f - NoH * NoH * (1.0f - a2), 2.0f);
                                float pdf = std::max(1e-8f, a2 * NoH / (denom * 4.0f));
                                AccumulateSample(l, pdf);
                            }
                        }
                        dst[y * mipWidth + x] = weight > 0.0f ? filtered / weight : float4(0.0f);
                    }
            }));
    }
    return result;
}
