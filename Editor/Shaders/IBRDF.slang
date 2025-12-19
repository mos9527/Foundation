/**
* PBRTv4-style interface for physically accurate Bidirectional Scattering Distribution Functions (BSDFs)
* ---
* Notes may be inaccurate as it's my own interpretation of the book. Please
* refer to "Physically Based Rendering, 4th Edition" at https://www.pbr-book.org/4ed/ for definitive explanations!
*/
import IMath;
// -- BxDF Interface
// https://www.pbr-book.org/4ed/Reflection_Models/BSDF_Representation
public struct BSDFSample {
    // Value of the BSDF
    public float3 f;
    // Incoming direction
    public float3 wi;
    // Sampler's PDF
    public float pdf;

    public __init(float3 f, float3 wi, float pdf) {
        this.f = f;
        this.wi = wi;
        this.pdf = pdf;
    }
    public bool IsValid() {
        return pdf >= 0.0;
    }
};
typedef float3 SampledSpectrum; // RGB spectrum
// https://www.pbr-book.org/4ed/Reflection_Models/BSDF_Representation#BxDFInterface
// * wo, wi are in local space (+z is normal direction)
public interface IBxDF {
    // Value of the distribution function for given pair of directions
    public float3 f(float3 wo, float3 wi);

    // [Importance] Sample a direction wi given outgoing direction wo and 2D random, uniform
    // samples uc and u.
    public BSDFSample Sample_f(float3 wo, float uc, float2 u);

    // Evaluates the PDF for a given pair of directions
    public float PDF(float3 wo, float3 wi);
};
// -- Spherical Geometry Utilities
public float AbsDot(float2 v1, float2 v2) { return abs(dot(v1, v2)); }
public float AbsDot(float3 v1, float3 v2) { return abs(dot(v1, v2)); }
public float Sqr(float v) { return v * v; }
public float LengthSquared(float2 v) { return dot(v, v); }
public float LengthSquared(float3 v) { return dot(v, v); }
public float SafeSqrt(float x) { return sqrt(max(0.f, x)); }
public float CosTheta(float3 w) { return w.z; }
public float Cos2Theta(float3 w) { return w.z * w.z; }
public float AbsCosTheta(float3 w) { return abs(w.z); }
public float Sin2Theta(float3 w) { return max(0, 1 - Cos2Theta(w)); }
public float SinTheta(float3 w) { return sqrt(Sin2Theta(w)); }
public float TanTheta(float3 w) { return SinTheta(w) / CosTheta(w); }
public float Tan2Theta(float3 w) { return Sin2Theta(w) / Cos2Theta(w); }
public float CosPhi(float3 w) {
    float sinTheta = SinTheta(w);
    return (sinTheta == 0) ? 1 : clamp(w.x / sinTheta, -1, 1);
}
public float SinPhi(float3 w) {
    float sinTheta = SinTheta(w);
    return (sinTheta == 0) ? 0 : clamp(w.y / sinTheta, -1, 1);
}
public float CosDPhi(float3 wa, float3 wb) {
    float waxy = Sqr(wa.x) + Sqr(wa.y), wbxy = Sqr(wb.x) + Sqr(wb.y);
    if (waxy == 0 || wbxy == 0) return 1;
    return clamp((wa.x * wb.x + wa.y * wb.y) / sqrt(waxy * wbxy),
                 -1, 1);
}
public bool SameHemisphere(float3 w, float3 wp) {
    return w.z * wp.z > 0.0;
}
// -- Sampling
// https://pbr-book.org/4ed/Sampling_Algorithms/Sampling_Multidimensional_Functions#SamplingaUnitDisk
public float2 SampleUniformDiskPolar(float2 u){
    float r = sqrt(u.x);
    float theta = 2 * Pi * u.y;
    return float2(r * cos(theta), r * sin(theta));
}
// https://pbr-book.org/4ed/Sampling_Algorithms/Sampling_Multidimensional_Functions#SamplingaUnitDisk
public float2 SampleConcentricDisk(float2 u) {
    float2 off = 2.0 * u - 1.0;
    if (off.x == 0 && off.y == 0) return 0;
    float theta, r;
    if (abs(off.x) > abs(off.y)) {
        r = off.x;
        theta = (Pi / 4) * (off.y / off.x);
    } else {
        r = off.y;
        theta = (Pi / 2) - (Pi / 4) * (off.x / off.y);
    }
    return r * float2(cos(theta), sin(theta));
}
// https://pbr-book.org/4ed/Sampling_Algorithms/Sampling_Multidimensional_Functions#UniformlySamplingHemispheresandSpheres
public float3 SampleUniformSphere(float2 u) {
    float z = 1 - 2 * u[0];
    float r = SafeSqrt(1 - Sqr(z));
    float phi = 2 * Pi * u[1];
    return float3(r * cos(phi), r * (phi), z);
}
// https://pbr-book.org/4ed/Sampling_Algorithms/Sampling_Multidimensional_Functions#Cosine-WeightedHemisphereSampling
public float3 SampleCosineHemisphere(float2 u) {
    float2 d = SampleConcentricDisk(u);
    float z = SafeSqrt(1.0 - Sqr(d.x) - Sqr(d.y));
    return float3(d.x, d.y, z);
}
public float CosineHemispherePDF(float cosTheta) {
    return cosTheta * InvPi;
}
// https://www.pbr-book.org/4ed/Reflection_Models/Roughness_Using_Microfacet_Theory
// Trowbridge-Reitz (GGX) distribution/shadow-masking functions
public struct TrowbridgeReitzDistribution {
    float alpha_x;
    float alpha_y;
    public __init(float alpha_x, float alpha_y) {
        this.alpha_x = alpha_x;
        this.alpha_y = alpha_y;
    }
    // https://www.pbr-book.org/4ed/Reflection_Models/Roughness_Using_Microfacet_Theory#eq:tr-d-function
    // Distribution/percentage of microfacet normals at surface local point oriented towards wm
    public float D(float3 wm) {
        float tan2Theta = Tan2Theta(wm);
        if (isinf(tan2Theta)) return 0;
        float cos4Theta = Sqr(Cos2Theta(wm));
        float e = tan2Theta * (Sqr(CosPhi(wm) / alpha_x) +
                               Sqr(SinPhi(wm) / alpha_y));
        return 1 / (Pi * alpha_x * alpha_y * cos4Theta * Sqr(1 + e));
    }
    // Fallback to perfectly smooth case where PDF is dirac delta when true
    public bool EffectivelySmooth() {
        return max(alpha_x, alpha_y) < 1e-3f;
    }
    // Lambda for G1 masking-shadowing function at incident direction w
    public float Lambda(float3 w) {
        float tan2Theta = Tan2Theta(w);
        if (isinf(tan2Theta)) return 0;
        float alpha2 = Sqr(CosPhi(w) * alpha_x) + Sqr(SinPhi(w) * alpha_y);
        return (sqrt(1 + alpha2 * tan2Theta) - 1) / 2;
    }
    // Masking function for single incident direction
    public float G1(float3 w) { return 1 / (1 + Lambda(w)); }
    // HEIGHT-CORRELATED MASKING-SHADOWING (SMITH G)
    // ---
    // * G1(wo)G1(wi) assumes independence - which is conservative and can lead to energy loss/darkening
    // * This correlates height fields - assumed as a NDF - to reduce energy loss
    public float G(float3 wo, float3 wi) {
        return 1 / (1 + Lambda(wo) + Lambda(wi));
    }
    // NORMALIZED VIEWING NORMAL DISTRIBUTION FUNCTION
    // ---
    // For visible microfacets viewed from wm:
    //   \int_H^2 G1(w)G1(wm) max(0,w \cdot wm) dwm = w \cdot n = CosTheta(w) should hold
    // Thus we can derive the PDF for sampling visible normals with respect to incident direction w:
    //   D_w(wm) = \frac{G1(w)}{CosTheta(wm)} D(wm) max(0,w \cdot wm)
    public float D(float3 w, float3 wm) {
        return G1(w) / AbsCosTheta(w) * D(wm) * AbsDot(w, wm);
    }
    // Alias of D, the PDF for visible normal's importance sampling described below.
    public float PDF(float3 w, float3 wm) { return D(w, wm); }
    // IMPORTANCE SAMPLING OF VISIBLE NORMALS
    // ---
    // https://www.pbr-book.org/4ed/Reflection_Models/Roughness_Using_Microfacet_Theory#SamplingtheDistributionofVisibleNormals
    // See also "Sampling the GGX Distribution of Visible Normals" by Eric Heitz (https://jcgt.org/published/0007/04/01/paper.pdf)
    public float3 Sample_wm(float3 w, float2 u) {
        // 1. For anisotropic cases, transform the incident w back to a hemispherical configuration
        //    so we'd always work with a isotropic case.
        float3 wh = normalize(float3(
            alpha_x * w.x,
            alpha_y * w.y,
            w.z
        ));
        if (wh.z < 0) wh = -wh; // Ensure wh is in the upper hemisphere
        // 2. Find a orthonormal basis around wh. This is PBRT's routine, though
        //    buildOrthonormalBasis [Frisvad, 2012] could be also used w/o normalization.
        float3 T1 = wh.z < 0.99999f ? normalize(cross(float3(0,0,1), wh)) : float3(1,0,0);
        float3 T2 = cross(wh, T1);
        // 3. Sample uniformly distributed point on a unit disk and project to clipped hemisphere
        float2 p = SampleUniformDiskPolar(u);
        float h = sqrt(1 - Sqr(p.x)); // Max height on hemisphere
        p.y = lerp((1 + wh.z) / 2, h, p.y); // Project to clipped hemisphere
        float pz = sqrt(max(0, 1 - LengthSquared(p)));
        float3 nh = p.x * T1 + p.y * T2 + pz * wh; // Apply TBN
        // 4. Reverse the anisotropic scaling to get the sampled microfacet normal
        return normalize(float3(alpha_x * nh.x, alpha_y * nh.y, max(EPS, nh.z)));
    }
}
// -- BSDFs
public struct DiffuseBxDF : IBxDF {
    SampledSpectrum R;
    public __init(SampledSpectrum R) {
        this.R = R;
    }
    // Constant reflection distribution where:
    // \int_H^2 f(wo,wi) CosTheta(wi) dwi = R
    public float3 f(float3 wo, float3 wi) {
        if (!SameHemisphere(wo, wi)) {
            return float3(0.0, 0.0, 0.0);
        }
        return R * InvPi;
    }
    // Draw a sample from cosine-weighted hemisphere
    public BSDFSample Sample_f(float3 wo, float uc, float2 u) {
        float3 wi = SampleCosineHemisphere(u);
        if (wo.z < 0.0) wi.z *= -1.0; // Ensure same hemisphere
        float pdf = CosineHemispherePDF(AbsCosTheta(wi));
        return BSDFSample(R * InvPi, wi, pdf);
    }
    // PDF for a pair of directions
    public float PDF(float3 wo, float3 wi) {
        if (!SameHemisphere(wo, wi))
            return 0.0;
        return CosineHemispherePDF(AbsCosTheta(wi));
    }
};
