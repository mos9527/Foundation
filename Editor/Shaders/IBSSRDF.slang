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

public struct BSSRDFSurfaceInteraction
{
    public float3 position;
    public float3 normal;
    public float3 tangent;
    public float3 bitangent;

    public __init(float3 position = float3(0.0f), float3 normal = float3(0.0f, 0.0f, 1.0f),
                  float3 tangent = float3(1.0f, 0.0f, 0.0f), float3 bitangent = float3(0.0f, 1.0f, 0.0f))
    {
        this.position = position;
        this.normal = normal;
        this.tangent = tangent;
        this.bitangent = bitangent;
    }
}

public struct BSSRDFSampleRaySegment
{
    public bool valid;
    public float3 origin;
    public float3 direction;
    public float tMax;

    public __init(bool valid = false, float3 origin = float3(0.0f),
                  float3 direction = float3(0.0f, 0.0f, 1.0f), float tMax = 0.0f)
    {
        this.valid = valid;
        this.origin = origin;
        this.direction = direction;
        this.tMax = tMax;
    }
}

public interface ISeparableBSSRDF
{
    // Radial profile evaluated at distance r.
    public float3 Sr(float r);
    public float3 Sp(BSSRDFSurfaceInteraction po, BSSRDFSurfaceInteraction pi);
    public BSSRDFSampleRaySegment Sample_SpRay(
        BSSRDFSurfaceInteraction po, float uAxis, float uChannel, float uRadius, float uPhi);
    public float Sample_Sr(uint channel, float u);
    public float Pdf_Sr(uint channel, float r);
    public float Pdf_Sp(BSSRDFSurfaceInteraction po, BSSRDFSurfaceInteraction pi);
}

// 2.4 Subsurface diffusion [Burley15]
public struct DisneyBurleyBSSRDF : ISeparableBSSRDF
{
    public float3 reflectance;
    public float3 scatterDistance;

    public __init(float3 reflectance = float3(1.0f), float3 scatterDistance = float3(1.0f))
    {
        this.reflectance = reflectance;
        // pbrt applies this 0.2 scale based on Burley/Chiang guidance.
        this.scatterDistance = max(0.2f * scatterDistance, float3(EPS));
    }

    // Exp fit of eq. (5), normalized to 1 over an infinite _surface_...
    public float3 Sr(float r)
    {
        r = max(r, 1e-6f);
        float3 d = scatterDistance;
        return reflectance * (exp(-float3(r) / d) + exp(-float3(r) / (3.0f * d))) /
               (8.0f * Pi * d * r);
    }

    public float3 Sp(BSSRDFSurfaceInteraction po, BSSRDFSurfaceInteraction pi)
    {
        return Sr(length(po.position - pi.position));
    }

    public BSSRDFSampleRaySegment Sample_SpRay(
        BSSRDFSurfaceInteraction po, float uAxis, float uChannel, float uRadius, float uPhi)
    {
        float3 vx, vy, vz;
        if (uAxis < 0.5f)
        {
            vx = po.tangent;
            vy = po.bitangent;
            vz = po.normal;
        }
        else if (uAxis < 0.75f)
        {
            vx = po.bitangent;
            vy = po.normal;
            vz = po.tangent;
        }
        else
        {
            vx = po.normal;
            vy = po.tangent;
            vz = po.bitangent;
        }

        uint channel = min(uint(uChannel * 3.0f), 2u);
        float r = Sample_Sr(channel, uRadius);
        float rMax = Sample_Sr(channel, 0.999f);
        if (r < 0.0f || r >= rMax)
            return BSSRDFSampleRaySegment();

        float phi = 2.0f * Pi * uPhi;
        float segmentLength = 2.0f * sqrt(max(rMax * rMax - r * r, 0.0f));
        if (segmentLength <= EPS_TMin)
            return BSSRDFSampleRaySegment();

        float3 origin = po.position +
                        r * (vx * cos(phi) + vy * sin(phi)) -
                        0.5f * segmentLength * vz;

        return BSSRDFSampleRaySegment(true, origin, vz, segmentLength);
    }

    // ...onto a radial profile, which would be Pr = 2pi * r * Sr (reflectance omitted)
    // Pr = (exp(-r/d) + exp(-r/(3d))) / (4d)
    //    = 0.25 * exp(-r/d) / d + 0.75 * exp(-r/(3d)) / (3d)
    // Integrate Pr to get CDF, which is trivial:
    //  F = 0.25 * (1 - exp(-r/d)) + 0.75 * (1 - exp(-r/(3d)))
    // Inversion can also be done in closed form:
    //  - Let x = exp(-r / (3d)), we have F = u = 1 - 0.25 x^3 - 0.75 x
    //  - x^3 + 3x = 4(1 - F)
    // Solve the cubic, one real root. You can go with closed form one like this via hyperbolics,
    // Note that [sinh(3a) = 3sinh(a) + 4sinh^3(a)] holds:
    //  - r = -3d * log(2sinh(asinh(2(1 - u)) / 3))
    // Or go with MIS. Indivdual Exp 'lobes' are nice to invert:
    //  - [0.25 part] r = d * log(1 / (1 - u))
    //  - [0.75 part] r = 3d * log(1 / (1 - u))
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

    // PDF can be then derived via balance heuristic, again it's simply a weighted sum here.
    public float Pdf_Sr(uint channel, float r)
    {
        r = max(r, 1e-6f); // Avoid singularity at r == 0.

        // Weight the two individual PDFs as per the sampling frequency in
        // Sample_Sr().
        float d = scatterDistance[channel];
        return 0.25f * exp(-r / d) / (2.0f * Pi * d * r) +
               0.75f * exp(-r / (3.0f * d)) / (6.0f * Pi * d * r);
    }

    public float Pdf_Sp(BSSRDFSurfaceInteraction po, BSSRDFSurfaceInteraction pi)
    {
        float3 d = po.position - pi.position;
        float3 dLocal = float3(dot(po.tangent, d), dot(po.bitangent, d), dot(po.normal, d));
        float3 nLocal = float3(dot(po.tangent, pi.normal),
                               dot(po.bitangent, pi.normal),
                               dot(po.normal, pi.normal));

        float rProjX = sqrt(dLocal.y * dLocal.y + dLocal.z * dLocal.z);
        float rProjY = sqrt(dLocal.z * dLocal.z + dLocal.x * dLocal.x);
        float rProjZ = sqrt(dLocal.x * dLocal.x + dLocal.y * dLocal.y);

        float pdf = 0.0f;
        const float chProb = 1.0f / 3.0f;
        for (uint ch = 0u; ch < 3u; ++ch)
        {
            pdf += 0.25f * chProb * abs(nLocal.x) * Pdf_Sr(ch, rProjX);
            pdf += 0.25f * chProb * abs(nLocal.y) * Pdf_Sr(ch, rProjY);
            pdf += 0.50f * chProb * abs(nLocal.z) * Pdf_Sr(ch, rProjZ);
        }
        return pdf;
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
//   - 2.3 Dielectric BRDF with integrated subsurface scattering [Burley15]
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

    float DisneySchlickWeight(float cosTheta)
    {
        float m = clamp(1.0f - cosTheta, 0.0f, 1.0f);
        return m * m * m * m * m;
    }

    public SampledSpectrum f(float3 wo, float3 wi, TransportMode mode)
    {
        if (!SameHemisphere(wo, wi)) return 0.0f;

        float3 wh = wi + wo;
        if (LengthSquared(wh) < EPS) return 0.0f;
        wh = normalize(wh);

        float cosThetaD = dot(wi, wh);
        float fss90 = cosThetaD * cosThetaD * roughness;
        // 'DisneyRetro'
        float Fo = DisneySchlickWeight(AbsCosTheta(wo)); // FL
        float Fi = DisneySchlickWeight(AbsCosTheta(wi)); // FV
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
