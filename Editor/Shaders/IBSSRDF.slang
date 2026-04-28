// From PBRT-v3
// https://pbr-book.org/3ed-2018/Volume_Scattering/The_BSSRDF
// References:
// - Brent Burley, "Extending the Disney BRDF to a BSDF with Integrated
//   Subsurface Scattering"
//   https://blog.selfshadow.com/publications/s2015-shading-course/burley/s2015_pbs_disney_bsdf_notes.pdf
// - pbrt-v3 Disney material implementation
//   https://github.com/mmp/pbrt-v3/blob/master/src/materials/disney.cpp
// The profile functions are the separable BSSRDF radial model from
// "Extending the Disney BRDF to a BSDF with integrated subsurface scattering".


import IBxDF;
import IMath;
public float DisneySchlickWeight(float cosTheta)
{
    float m = clamp(1.0f - cosTheta, 0.0f, 1.0f);
    return m * m * m * m * m;
}

public struct DisneyBurleyBSSRDF
{
    public float3 reflectance;
    public float3 scatterDistance;

    public __init(float3 reflectance = float3(1.0f), float3 scatterDistance = float3(1.0f))
    {
        this.reflectance = reflectance;
        // pbrt applies this 0.2 scale based on Burley/Chiang guidance.
        this.scatterDistance = max(0.2f * scatterDistance, float3(EPS));
    }

    // Diffusion profile from Burley 2015, eq. (5).
    public float3 Sr(float r)
    {
        r = max(r, 1e-6f);
        float3 d = scatterDistance;
        return reflectance * (exp(-float3(r) / d) + exp(-float3(r) / (3.0f * d))) /
               (8.0f * Pi * d * r);
    }

    public float Sample_Sr(uint channel, float u)
    {
        float d = scatterDistance[channel];
        if (u < 0.25f)
        {
            u = min(u * 4.0f, 0.99999994f);
            return d * log(1.0f / (1.0f - u));
        }

        u = min((u - 0.25f) / 0.75f, 0.99999994f);
        return 3.0f * d * log(1.0f / (1.0f - u));
    }

    public float Pdf_Sr(uint channel, float r)
    {
        r = max(r, 1e-6f);
        float d = scatterDistance[channel];
        return 0.25f * exp(-r / d) / (2.0f * Pi * d * r) +
               0.75f * exp(-r / (3.0f * d)) / (6.0f * Pi * d * r);
    }
};

public struct BSSRDFSurfaceCandidate : IReservoirCandidate
{
    public uint32_t instance;
    public uint32_t primitive;
    public float2 bary;
    public float hitT;

    public __init(uint32_t instance = ~0u, uint32_t primitive = ~0u,
                  float2 bary = float2(0.0f), float hitT = 0.0f)
    {
        this.instance = instance;
        this.primitive = primitive;
        this.bary = bary;
        this.hitT = hitT;
    }

    public bool IsValid()
    {
        return this.instance != ~0u;
    }
};

// NOTE: Unused
// "Fake" subsurface scattering lobe, based on the Hanrahan-Krueger BRDF
// approximation of the BSSRDF.
// See also
//   - https://github.com/mmp/pbrt-v3/blob/master/src/materials/disney.cpp#L123
//   - https://dl.acm.org/doi/10.1145/166117.166139 [Krueger93]
public struct DisneyFakeSS : IBxDF
{
    float3 color;
    float roughness;

    public __init(float3 color = float3(1.0f), float roughness = 0.5f)
    {
        this.color = color;
        this.roughness = roughness;
    }

    public BxDFFlags Flags() { return BxDFFlags::DiffuseReflection; }

    public SampledSpectrum f(float3 wo, float3 wi, TransportMode mode)
    {
        if (!SameHemisphere(wo, wi)) return 0.0f;

        float3 wh = wi + wo;
        if (LengthSquared(wh) < EPS) return 0.0f;
        wh = normalize(wh);

        float cosThetaD = dot(wi, wh);
        float fss90 = cosThetaD * cosThetaD * roughness;
        float Fo = DisneySchlickWeight(AbsCosTheta(wo));
        float Fi = DisneySchlickWeight(AbsCosTheta(wi));
        float Fss = lerp(1.0f, fss90, Fo) * lerp(1.0f, fss90, Fi);

        float cosSum = max(AbsCosTheta(wo) + AbsCosTheta(wi), EPS);
        float ss = 1.25f * (Fss * (1.0f / cosSum - 0.5f) + 0.5f);
        return color * InvPi * ss;
    }

    public BSDFSample Sample_f(float3 wo, float uc, float2 u, TransportMode mode, BxDFReflTransFlags flags)
    {
        if (!(flags & BxDFReflTransFlags::Reflection)) return BSDFSample();

        float3 wi = SampleCosineHemisphere(u);
        wi = FaceForward(wi, float3(0, 0, 1));
        float pdf = CosineHemispherePDF(AbsCosTheta(wi));
        return BSDFSample(this.f(wo, wi, mode), wi, pdf, BxDFFlags::DiffuseReflection);
    }

    public float PDF(float3 wo, float3 wi, TransportMode mode, BxDFReflTransFlags flags)
    {
        if (!(flags & BxDFReflTransFlags::Reflection) || !SameHemisphere(wo, wi)) return 0.0f;
        return CosineHemispherePDF(AbsCosTheta(wi));
    }
};
