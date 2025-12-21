/**
* PBRTv4-style interface for physically accurate Bidirectional Scattering Distribution Functions (BSDFs)
* ---
* Notes may be inaccurate as it's my own interpretation of the book. Please
* refer to "Physically Based Rendering, 4th Edition" at https://www.pbr-book.org/4ed/ for definitive explanations!
*/
import IMath;
// -- BxDF Interface
public enum BxDFReflTransFlags {
    Unset = 0,
    Reflection = 1 << 0,
    Transmission = 1 << 1,
    All = 0b11
}
public enum BxDFFlags {
    Unset = 0,
    Reflection = 1 << 0,    // 0b00001
    Transmission = 1 << 1,  // 0b00010
    Diffuse = 1 << 2,       // 0b00100
    Glossy = 1 << 3,        // 0b01000, Rough surface
    Specular = 1 << 4,      // 0b10000, Perfect mirror-like
    // Composite types
    DiffuseReflection = 0b00101,
    DiffuseTransmission = 0b00110,
    GlossyReflection = 0b01001,
    GlossyTransmission = 0b01010,
    SpecularReflection = 0b10001,
    SpecularTransmission = 0b10010,
    All = 0b11111
}
public bool IsReflection(BxDFFlags f) { return f & BxDFFlags::Reflection; }
public bool IsTransmission(BxDFFlags f) { return f & BxDFFlags::Transmission; }
public bool IsDiffuse(BxDFFlags f) { return f & BxDFFlags::Diffuse; }
public bool IsGlossy(BxDFFlags f) { return f & BxDFFlags::Glossy; }
public bool IsSpecular(BxDFFlags f) { return f & BxDFFlags::Specular; }
public bool IsNonSpecular(BxDFFlags f) {
    return f & (BxDFFlags::Diffuse | BxDFFlags::Glossy);
 }
public enum TransportMode {
    Radiance, // Towards camera
    Importance // Toward light source for non-symmetric BSDFs
}
// https://www.pbr-book.org/4ed/Reflection_Models/BSDF_Representation
public struct BSDFSample {
    // Value of the BSDF
    public float3 f;
    // Incoming direction
    public float3 wi;
    // Sampler's PDF
    // For dirac delta distributions, this should be 1 to match
    // that of the BxDFs sampled.
    public float pdf;
    // Which type of BxDF was sampled - useful for MIS
    BxDFFlags flags;
    // For transmission BxDFs, the relative IOR between
    // incident and transmitted mediums
    float eta;

    public __init(float3 f = float3(0), float3 wi = float3(0), float pdf = 0.0, BxDFFlags flags = BxDFFlags::Unset, float eta = 1.0f) {
        this.f = f;
        this.wi = wi;
        this.pdf = pdf;
        this.flags = flags;
        this.eta = eta;
    }
    public bool IsValid() {
        return pdf > 0.0 && flags != BxDFFlags::Unset;
    }
};
// https://pbr-book.org/4ed/Volume_Scattering/Phase_Functions#
public struct PhaseFunctionSample {
    float p;
    float3 wi;
    float pdf;
    public __init(float p = 0.0, float3 wi = float3(0), float pdf = 0.0) {
        this.p = p;
        this.wi = wi;
        this.pdf = pdf;
    }
};
typedef float3 SampledSpectrum; // RGB spectrum
// https://www.pbr-book.org/4ed/Reflection_Models/BSDF_Representation#BxDFInterface
// * wo, wi are in local space (+z is normal direction)
public interface IBxDF {
    // Evaluates the BxDF for given pair of directions
    public SampledSpectrum f(float3 wo, float3 wi, TransportMode mode = TransportMode::Radiance);

    // Samples the BxDF given outgoing direction wo and 2D random, uniform
    // samples uc and u.
    // PDF of the sampler is returned in the BSDFSample struct,
    // which should be a form of Importance Sampling and thus non-zero.
    public BSDFSample Sample_f(float3 wo, float uc, float2 u, TransportMode mode = TransportMode::Radiance, BxDFReflTransFlags flags = BxDFReflTransFlags::All);

    // Evaluates the PDF of the BxDF for a given pair of directions, used in MIS
    // Returns 1 for dirac delta distributions
    public float PDF(float3 wo, float3 wi, TransportMode mode = TransportMode::Radiance, BxDFReflTransFlags flags = BxDFReflTransFlags::All);

    // Returns the type flags of this BxDF
    public BxDFFlags Flags();
};
// -- Spherical Geometry Utilities
public float MaxComponent(float3 v) { return max(max(v.x, v.y), v.z); }
public float MaxComponent(float2 v) { return max(v.x, v.y); }
public float AbsDot(float2 v1, float2 v2) { return abs(dot(v1, v2)); }
public float AbsDot(float3 v1, float3 v2) { return abs(dot(v1, v2)); }
public float Sqr(float v) { return v * v; }
public complex Sqr(complex v) { return v * v; }
public float LengthSquared(float2 v) { return dot(v, v); }
public float LengthSquared(float3 v) { return dot(v, v); }
public float SafeSqrt(float x) { return sqrt(max(0.f, x)); }
public float CosTheta(float3 w) { return w.z; }
public float Cos2Theta(float3 w) { return w.z * w.z; }
public float AbsCosTheta(float3 w) { return abs(w.z); }
public float ClampedCosTheta(float3 w) { return clamp(w.z, 0, 1); }
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
public float3 FaceForward(float3 n, float3 v) {
    return dot(v, n) < 0.0 ? -n : n;
}
public float3 SphericalDirection(float sinTheta, float cosTheta, float phi) {
    return float3(clamp(sinTheta, -1, 1) * cos(phi), clamp(sinTheta, -1, 1) * sin(phi), clamp(cosTheta, -1, 1));
}
// -- Light Transport
// GLSL/HLSL reflect()
// Inverse is n = normalize(wo + wr)
public float3 Reflect(float3 wo, float3 n) {
    //     vvv wo is flipped (pointing away)
    return -wo + 2 * dot(wo, n) * n;
}
// https://www.pbr-book.org/4ed/Reflection_Models/Specular_Reflection_and_Transmission#SnellrsquosLaw
public bool Refract(float3 wi, float3 n, float eta /* IOR */, out float3 wt, out float etap){
    float cosTheta_i = dot(n, wi);
    if (cosTheta_i < 0) { // Inside an object?
        eta = 1 / eta;
        cosTheta_i = -cosTheta_i;
        n = -n;
    }
    float sin2Theta_i = 1 - Sqr(cosTheta_i);
    float sin2Theta_t = sin2Theta_i / Sqr(eta);
    if (sin2Theta_t >= 1) // Total internal reflection - transmission impossible
        return false;
    float cosTheta_t = SafeSqrt(1 - sin2Theta_t);
    wt = eta * -wi + (eta * cosTheta_i - cosTheta_t) * n;
    etap = eta;
    return true;
}
// https://www.pbr-book.org/4ed/Reflection_Models/Specular_Reflection_and_Transmission#TheFresnelEquations
// Percentage of light reflected (or otherwise transmitted) at a dielectric interface
// with respect of incident angle theta and IOR eta
public float FrDielectric(float cosTheta_i, float eta) {
    // vv Same snippet for calculating Snell's law
    if (cosTheta_i < 0) { // Inside an object?
        eta = 1 / eta;
        cosTheta_i = -cosTheta_i;
    }
    float sin2Theta_i = 1 - Sqr(cosTheta_i);
    float sin2Theta_t = sin2Theta_i / Sqr(eta);
    if (sin2Theta_t >= 1) // Total internal reflection
        return 1.0f; // All scattering is in the reflection, no transmission component
    float cosTheta_t = SafeSqrt(1 - sin2Theta_t);
    // ^^
    float r_parl = (eta * cosTheta_i - cosTheta_t) /
                   (eta * cosTheta_i + cosTheta_t);
    float r_perp = (cosTheta_i - eta * cosTheta_t) /
                   (cosTheta_i + eta * cosTheta_t);
   return (Sqr(r_parl) + Sqr(r_perp)) / 2; // Power of both polarizations
}
// https://www.pbr-book.org/4ed/Reflection_Models/Specular_Reflection_and_Transmission#TheFresnelEquationsforConductors
// eta takes a complex form (IOR, k), where k is the absorption coefficient
public float FrComplex(float cosTheta_i, complex eta){
    cosTheta_i = clamp(cosTheta_i, 0, 1);
    // ^^ Ignore the case of inside an object for conductors as it's attenuated rapidly
    float sin2Theta_i = 1 - Sqr(cosTheta_i);
    complex sin2Theta_t = complex(sin2Theta_i) / Sqr(eta);
    complex cosTheta_t = sqrt(complex(1) - sin2Theta_t);
    complex r_parl = (eta * complex(cosTheta_i) - cosTheta_t) /
                     (eta * complex(cosTheta_i) + cosTheta_t);
    complex r_perp = (complex(cosTheta_i) - eta * cosTheta_t) /
                     (complex(cosTheta_i) + eta * cosTheta_t);
    return (norm(r_parl) + norm(r_perp)) / 2; // Power of both polarizations
}
// Helper for varying IOR per RGB spectra
public SampledSpectrum FrComplex(float cosTheta_i, SampledSpectrum eta, SampledSpectrum k) {
    float3 result;
    for (int i = 0; i < 3; i++)
        result[i] = FrComplex(cosTheta_i, complex(eta[i], k[i]));
    return result;
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
    return float3(r * cos(phi), r * sin(phi), z);
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
    // Height-correlated Masking-Shadowing function (Smith G)
    // ---
    // * G1(wo)G1(wi) assumes independence - which is conservative and can lead to energy loss/darkening
    // * This correlates height fields - assumed as a NDF - to reduce energy loss
    public float G(float3 wo, float3 wi) {
        return 1 / (1 + Lambda(wo) + Lambda(wi));
    }
    // Normalized Visible Normal Distribution/VNDF
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
    // Importance sampling of Visible Normals
    // ---
    // https://www.pbr-book.org/4ed/Reflection_Models/Roughness_Using_Microfacet_Theory#SamplingtheDistributionofVisibleNormals
    // See also "Sampling the GGX Distribution of Visible Normals" by Eric Heitz (https://jcgt.org/published/0007/04/01/paper.pdf)
    public float3 Sample_wm(float3 w, float2 u) {
        // 1. For anisotropic cases, transform the incident w back to a hemispherical configuration
        //    so we'd always work with a isotropic case.
        float3 wh = normalize(float3(alpha_x * w.x, alpha_y * w.y, w.z));
        if (wh.z < 0) wh = -wh; // Ensure wh is in the upper hemisphere
        // 2. Find a orthonormal basis around wh. This is PBRT's routine, though
        //    buildOrthonormalBasis [Frisvad, 2012] could be also used w/o normalization.
        float3 T1 = wh.z < 0.99999f ? normalize(cross(float3(0,0,1), wh)) : float3(1,0,0);
        float3 T2 = cross(wh, T1);
        // 3. Sample uniformly distributed point on a unit disk and project to clipped hemisphere
        float2 p = SampleUniformDiskPolar(u);
        float h = sqrt(1 - Sqr(p.x)); // Max height on hemisphere
        float s = (1 + wh.z) / 2;
        // !!! PBRT used a lerp here. This is undefined when p.y is outside [0,1] !!!
        // The original formulation is implemented here (t2 = (1.0 - s) * sqrt(1.0 - t1 * t1) + s * t2)
        p.y = (1-s) * h + s * p.y; // Project to clipped hemisphere
        float pz = sqrt(max(0, 1 - LengthSquared(p)));
        float3 nh = p.x * T1 + p.y * T2 + pz * wh; // Apply TBN
        // 4. Reverse the anisotropic scaling to get the sampled microfacet normal
        return normalize(float3(alpha_x * nh.x, alpha_y * nh.y, max(1e-6f, nh.z)));
    }
}
// https://pbr-book.org/4ed/Volume_Scattering/Phase_Functions#HenyeyGreenstein
float HenyeyGreenstein(float cosTheta, float g) {
    g = clamp(g, -.99, .99);
    float denom = 1 + Sqr(g) + 2 * g * cosTheta;
    return Inv4Pi * (1 - Sqr(g)) / (denom * SafeSqrt(denom));
}
float3 SampleHenyeyGreenstein(float3 wo, float g, float2 u, out float pdf) {
    g = clamp(g, -.99, .99);

    float cosTheta;
    if (abs(g) < 1e-3f)
        cosTheta = 1 - 2 * u[0];
    else
        cosTheta = -1 / (2 * g) * (1 + Sqr(g) - Sqr((1 - Sqr(g)) / (1 + g - 2 * g * u[0])));
    pdf = HenyeyGreenstein(cosTheta, g);

    // Compute direction _wi_ for Henyey--Greenstein sample
    float sinTheta = SafeSqrt(1 - Sqr(cosTheta));
    float phi = 2 * Pi * u[1];
    float3 t1, t2;
    buildOrthonormalBasis(wo, t1, t2);
    float3 wi = SphericalDirection(sinTheta, cosTheta, phi);
    return wi.x * wo + wi.y * t1 + wi.z * t2;
}
public struct HGPhaseFunction {
    float g;
    public __init(float g) {
        this.g = g;
    }
    float p(float3 wo, float3 wi) {
        return HenyeyGreenstein(dot(wo, wi), g);
    }
    PhaseFunctionSample Sample_p(float3 wo, float2 u) {
        float pdf;
        float3 wi = SampleHenyeyGreenstein(wo, g, u, pdf);
        return PhaseFunctionSample(pdf, wi, pdf);
    }
    float PDF(float3 wo, float3 wi) {
        return HenyeyGreenstein(dot(wo, wi), g);
    }
}
// https://pbr-book.org/4ed/Sampling_Algorithms/Sampling_1D_Functions#SampleExponential
public float SampleExponential(float u, float a) {
    return -log(1 - u) / a;
}
// https://pbr-book.org/4ed/Monte_Carlo_Integration/Improving_Efficiency#PowerHeuristic
public float BalanceHeuristic(int nf, float fPdf, int ng, float gPdf) {
    return (nf * fPdf) / (nf * fPdf + ng * gPdf);
}
public float PowerHeuristic(int nf, float fPdf, int ng, float gPdf) {
    float f = nf * fPdf, g = ng * gPdf;
    return Sqr(f) / (Sqr(f) + Sqr(g));
}
// -- BSDFs
// https://www.pbr-book.org/4ed/Reflection_Models/Diffuse_Reflection
public struct DiffuseBxDF : IBxDF {
    SampledSpectrum R;
    public __init(SampledSpectrum R) {
        this.R = R;
    }
    public BxDFFlags Flags(){
        return BxDFFlags::DiffuseReflection;
    }
    // BRDF evaluation
    // Constant reflection distribution where:
    // \int_H^2 f(wo,wi) CosTheta(wi) dwi = R
    public SampledSpectrum f(float3 wo, float3 wi, TransportMode) {
        if (!SameHemisphere(wo, wi)) {
            return float3(0.0, 0.0, 0.0);
        }
        return R * InvPi;
    }
    // Draw a sample from cosine-weighted hemisphere
    public BSDFSample Sample_f(float3 wo, float uc, float2 u,TransportMode, BxDFReflTransFlags flags) {
        if (!(flags & BxDFReflTransFlags::Reflection))
            return BSDFSample();
        float3 wi = SampleCosineHemisphere(u);
        if (wo.z < 0.0) wi.z *= -1.0; // Ensure same hemisphere
        float pdf = CosineHemispherePDF(AbsCosTheta(wi));
        return BSDFSample(R * InvPi, wi, pdf, BxDFFlags::DiffuseReflection);
    }
    // Evaluated PDF for a pair of directions
    public float PDF(float3 wo, float3 wi,TransportMode, BxDFReflTransFlags flags) {
        if (!(flags & BxDFReflTransFlags::Reflection) || !SameHemisphere(wo, wi))
            return 0.0;
        return CosineHemispherePDF(AbsCosTheta(wi));
    }
};
// https://www.pbr-book.org/4ed/Reflection_Models/Conductor_BRDF
public struct ConductorBxDF : IBxDF {
    SampledSpectrum eta; // IOR
    SampledSpectrum k; // Absorption coefficient
    TrowbridgeReitzDistribution mfDistrib; // Microfacet distribution
    public __init(SampledSpectrum eta, SampledSpectrum k, float alpha_x, float alpha_y) {
        this.eta = eta;
        this.k = k;
        this.mfDistrib = TrowbridgeReitzDistribution(alpha_x, alpha_y);
    }
    public BxDFFlags Flags(){
        if (mfDistrib.EffectivelySmooth())
            return BxDFFlags::SpecularReflection;
        else
            return BxDFFlags::GlossyReflection;
    }
    // Torrance-Sparrow PDF
    public float PDF(float3 wo, float3 wi, TransportMode, BxDFReflTransFlags flags) {
        if (!(flags & BxDFReflTransFlags::Reflection) || !SameHemisphere(wo, wi))
            return 0.0;
        if (mfDistrib.EffectivelySmooth())
            return 0.0; // Dirac delta. But for singular evaluations, we return 0 to avoid infinities.
        float3 wm = wo + wi;
        if (LengthSquared(wm) == 0)
            return 0.0;
        wm = FaceForward(normalize(wm), float3(0,0,1)); // Again, on a hemisphere.
        // Jacobian of the half-vector space (mfDistrib) to normal space conversion
        // --
        // https://www.pbr-book.org/4ed/Reflection_Models/Roughness_Using_Microfacet_Theory#x5-TheHalf-DirectionTransform
        // For square jacobian (R3->R3) here, both in spherical coords, we have:
        //   \frac{dwm}{dwi} = \frac{sin theta_m d theta_m d phi_m}{sin theta_i d theta_i d phi_i}
        // Recall that theta_r = theta_i and that on a flat 2D scenario:
        //   theta_m = (theta_i + theta_o) / 2
        // And in this case assume theta_o is infinitely small (viewing from normal direction):
        //   theta_m = theta_i / 2
        // Plugging it back, we have the simplified form:
        //  \frac{dwm}{dwi} = \frac{sin theta_m d theta_m d phi_m}{sin 2theta_m 2 d theta_m 2 d phi_m}
        // Which gives the final jacobian in the vvv denom vvv
        return mfDistrib.PDF(wo, wm) / (4 * AbsDot(wo, wm));
    }
    // Torrance-Sparrow BRDF Sampling
    public BSDFSample Sample_f(float3 wo, float uc, float2 u,TransportMode, BxDFReflTransFlags flags) {
        if (!(flags & BxDFReflTransFlags::Reflection))
            return BSDFSample();
        if (mfDistrib.EffectivelySmooth()){
            // https://www.pbr-book.org/4ed/Reflection_Models/Conductor_BRDF
            // Finding f
            // ---
            // Lobe is effectively a single ray, thus \theta_r = \theta_o -> reflection = outgoing
            //   wr = Reflect(wo, n) = (-wo.x, -wo.y, wo.z)  (normal frame is +Z)
            // Thus Lo = \int f(wi) * Li(wi) * cos(theta_i) dwi = Fr(wr)Li(wr) holds, we need a f.
            // Using a dirac delta to find f gives:
            //   f(wo, wi) = \delta(wi - wr) Fr(wi)
            // Integrate, we have an extra AbsCosTheta(wi):
            //   Lo = Fr(wr)Li(wr) AbsCosTheta(wr)
            // Removing it from the previous f would be then correct, thus
            //   f(wo, wi) = \delta(wi - wr) Fr(wi) / AbsCosTheta(wr)
            // Remember that cos theta_r = cos theta_i, which gives the following impl:
            float3 wi = float3(-wo.x, -wo.y, wo.z);
            SampledSpectrum F = FrComplex(AbsCosTheta(wi), eta, k) / AbsCosTheta(wi);
            return BSDFSample(F, wi, 1.0, BxDFFlags::SpecularReflection);
        } else {
            // https://www.pbr-book.org/4ed/Reflection_Models/Roughness_Using_Microfacet_Theory#TheTorrancendashSparrowModel
            // https://www.pbr-book.org/4ed/Reflection_Models/Roughness_Using_Microfacet_Theory#x5-TorrancendashSparrowSampling
            // Finding f
            // ---
            //   Lo = \int f(p, wo, wi) * Li(p, wi) * cos(theta_i) dwi ~ \frac{f(p, wo, wi) * Li(p, wi) * cos(theta_i)}{p(wi)}
            // Where the identity must hold in a microfacet context:
            //   Lo = F(wo * wm)G(wi,wo)Li(p,wi)
            // Recall the previous Torrance-Sparrow PDF derivation, we have:
            //   p(wi) = D_w(wo, wm) * AbsDot(wo, wm) / (4 * AbsDot(wo, wm))
            // Plugging everything in:
            //   f(p, wo, wi) = D(wo,wm) * F(wo * wm) * G(wi, wo) / (4 * AbsDot(wo, wm) * AbsCosTheta(wi))
            // And plugging the previous VNDF D, we have the simplified form:
            //   f(p, wo, wi) = D(wm) * F(wo * wm) * G(wi, wo) / (4 * AbsCosTheta(wi) * AbsCosTheta(wo))
            // ---
            // 1. Sample a microfacet normal given view (outgoing) direction and VNDF
            float3 wm = mfDistrib.Sample_wm(wo, u);
            float3 wi = Reflect(wo, wm);
            if (!SameHemisphere(wo, wi))
                return BSDFSample(); // Absorbed, or failed to scatter. Multi-scatter is required to solve this.
            // 2. Compute the scaled PDF for the sampled direction
            // Using Jacobian from previous derivation vvvv
            float pdf = mfDistrib.PDF(wo, wm) / (4 * AbsDot(wo, wm));
            float cosTheta_o = AbsCosTheta(wo), cosTheta_i = AbsCosTheta(wi);
            if (cosTheta_o == 0 || cosTheta_i == 0)
                return BSDFSample(); // Grazing
            // 3. Calculate Fresnel for amount of reflection
            SampledSpectrum F = FrComplex(AbsDot(wo, wm), eta, k);
            // 4. Final BSDF value following previous derivation
            SampledSpectrum f = mfDistrib.D(wm) * F * mfDistrib.G(wo, wi) /
                                (4 * cosTheta_i * cosTheta_o);
            return BSDFSample(f, wi, pdf, BxDFFlags::GlossyReflection);
       }
    }
   // BRDF evaluation
   public SampledSpectrum f(float3 wo, float3 wi, TransportMode) {
        if (!SameHemisphere(wo, wi)) return {};
        // For specular peaks - this is generally unreachable by sampled cases with e.g.
        // analytical lights - return 0 to avoid infinities/fireflies.
        if (mfDistrib.EffectivelySmooth()) return {};
        float cosTheta_o = AbsCosTheta(wo), cosTheta_i = AbsCosTheta(wi);
        if (cosTheta_i == 0 || cosTheta_o == 0) return 0; // Grazing
        float3 wm = wo + wi;
        if (LengthSquared(wm) == 0)
            return 0.0;
        wm = normalize(wm);
        SampledSpectrum F = FrComplex(AbsDot(wo, wm), eta, k);
        return mfDistrib.D(wm) * F * mfDistrib.G(wo, wi) /
                            (4 * cosTheta_i * cosTheta_o);
   }
};
// https://www.pbr-book.org/4ed/Reflection_Models/Dielectric_BSDF
public struct DielectricBxDF : IBxDF {
    float eta; // IOR
    TrowbridgeReitzDistribution mfDistrib; // Microfacet distribution
    public __init(float eta, float alpha_x, float alpha_y) {
        this.eta = eta;
        this.mfDistrib = TrowbridgeReitzDistribution(alpha_x, alpha_y);
    }
    public BxDFFlags Flags(){
        BxDFFlags flags = (eta == 1) ? BxDFFlags::Transmission : BxDFFlags::Transmission | BxDFFlags::Reflection;
        return flags | (mfDistrib.EffectivelySmooth() ? BxDFFlags::Specular : BxDFFlags::Glossy);
    }
   public float PDF(float3 wo, float3 wi, TransportMode, BxDFReflTransFlags flags) {
        if (eta == 1 || mfDistrib.EffectivelySmooth())
            return 0;
        // https://www.pbr-book.org/4ed/Reflection_Models/Rough_Dielectric_BSDF#sec:rough-dielectric
        // 1. Determine half-vector
        float cosTheta_o = CosTheta(wo), cosTheta_i = CosTheta(wi);
        // \frac{sin in}{sin out} > n -> Over critical angle -> no transmission, equivalent to IOR=1.0
        bool reflected = cosTheta_i * cosTheta_o > 0;
        float etap = 1;
        if (!reflected)
           etap = (cosTheta_o > 0) ? eta : 1 / eta;
        float3 wm = wi * etap + wo;
        if (cosTheta_i == 0 || cosTheta_o == 0 || LengthSquared(wm) == 0) return {};
        wm = FaceForward(normalize(wm), float3(0, 0, 1));
        // 2. Discard back-facing microfacets to either in/out directions
        if (dot(wm,wi) * cosTheta_i < 0 || dot(wm,wo) * cosTheta_o < 0)
           return 0;
        // 3. Determine reflectance
        float R = FrDielectric(dot(wo, wm), eta), T = 1 - R;
        // 4. Compute probabilities
        float pr = R, pt = T;
        if (!(flags & BxDFReflTransFlags::Reflection)) pr = 0;
        if (!(flags & BxDFReflTransFlags::Transmission)) pt = 0;
        float pdf;
        if (reflected){
            // Reflection only, same as Torrance-Sparrow but with probability weighting
            pdf = mfDistrib.PDF(wo, wm) / (4 * AbsDot(wo, wm)) * pr / (pr + pt);
        } else {
            // Another Jacobian for transmission case
            // ---
            // See formulation at:
            //   https://www.pbr-book.org/4ed/Reflection_Models/Rough_Dielectric_BSDF#eq:mf-transmission-change-of-vars
            float denom = Sqr(dot(wi, wm) + dot(wo,wm) / etap);
            float dwm_dwi = AbsDot(wi, wm) / denom;
            pdf = mfDistrib.PDF(wo, wm) * dwm_dwi * pt / (pr + pt);
        }
        return pdf;
   }
    public BSDFSample Sample_f(float3 wo, float uc, float2 u, TransportMode transport, BxDFReflTransFlags flags){
        if (eta == 1 || mfDistrib.EffectivelySmooth()){
            // Sample from either reflection or transmission based on Fresnel
            float R = FrDielectric(CosTheta(wo), eta), T = 1 - R;
            float pr = R, pt = T;
            if (!(flags & BxDFReflTransFlags::Reflection)) pr = 0;
            if (!(flags & BxDFReflTransFlags::Transmission)) pt = 0;
            if (pr == 0 && pt == 0)
                return BSDFSample();
            if (uc < pr / (pr + pt)){
                // Sample perfectly specular BRDF
                // The same as the ConductorBxDF's smooth case
                float3 wi = float3(-wo.x, -wo.y, wo.z);
                SampledSpectrum F = float3(R) / AbsCosTheta(wi);
                return BSDFSample(F, wi, pr / (pr + pt), BxDFFlags::SpecularReflection);
            } else {
                // Sample perfectly specular BTDF
                float etap; float3 wi;
                // vvv Shouldn't be possible to fail since T won't be 0 vvv
                if (!Refract(wo, float3(0,0,1), eta, wi, etap))
                    return BSDFSample();
                SampledSpectrum ft = float3(T) / AbsCosTheta(wi);
                // Adjust for non-symmetric transport
                // https://www.pbr-book.org/4ed/Reflection_Models/Dielectric_BSDF#Non-SymmetricScatteringandRefraction
                // ...I didn't read into this part, so I'll just copy the formula here.
                if (transport == TransportMode::Radiance)
                    ft /= Sqr(etap);
                return BSDFSample(ft, wi, pt / (pr + pt), BxDFFlags::SpecularTransmission, etap);
            }
        } else {
            // https://www.pbr-book.org/4ed/Reflection_Models/Rough_Dielectric_BSDF#x0-RoughDielectricSampling
            // 1. Sample microfacet normal from VNDF
            float3 wm = mfDistrib.Sample_wm(wo, u);
            float R = FrDielectric(dot(wo, wm), eta), T = 1 - R;
            float pr = R, pt = T;
            if (!(flags & BxDFReflTransFlags::Reflection)) pr = 0;
            if (!(flags & BxDFReflTransFlags::Transmission)) pt = 0;
            if (pr == 0 && pt == 0)
                return BSDFSample();
            if (uc < pr / (pr + pt)){
                // Sample rough BRDF
                float3 wi = Reflect(wo, wm);
                if (!SameHemisphere(wo, wi))
                    return BSDFSample(); // We're not in BTDF, so discard
                // 2. Compute PDF
                // Same Jacobian again, with scaling for prob selection
                float pdf = mfDistrib.PDF(wo, wm) / (4 * AbsDot(wo, wm)) * pr / (pr + pt);
                // 3. Compute final BRDF value
                float cosTheta_o = AbsCosTheta(wo), cosTheta_i = AbsCosTheta(wi);
                // vvv Note: Fresnel is accounted for in the prob selection vvv
                SampledSpectrum f = R * mfDistrib.D(wm) * mfDistrib.G(wo, wi) /
                                    (4 * cosTheta_i * cosTheta_o);
                return BSDFSample(f, wi, pdf, BxDFFlags::GlossyReflection);
            } else {
                // Sample rough BTDF
                float etap;
                float3 wi;
                if (!Refract(wo, wm, eta, wi, etap) || SameHemisphere(wo, wi) || wi.z == 0)
                    return BSDFSample(); // Total internal reflection or invalid sample
                // 2. Compute PDF
                // The same Jacobian for transmission case in PDF(), with prob scaling
                float denom = Sqr(dot(wi, wm) + dot(wo,wm) / etap);
                float dwm_dwi = AbsDot(wi, wm) / denom;
                float pdf = mfDistrib.PDF(wo, wm) * dwm_dwi * pt / (pr + pt);
                // 3. Compute final BTDF value
                SampledSpectrum f = T * mfDistrib.D(wm) * mfDistrib.G(wo, wi) *
                                    abs(dot(wi,wm) * dot(wo,wm)) /
                                    (CosTheta(wi) * CosTheta(wo) * denom);
                // 4. Adjust for non-symmetric transport
                if (transport == TransportMode::Radiance)
                    f /= Sqr(etap);
                return BSDFSample(f, wi, pdf, BxDFFlags::GlossyTransmission, etap);
            }
        }
   }
   public SampledSpectrum f(float3 wo, float3 wi, TransportMode transport) {
        // Same as the Conductor case again.
        if (eta == 1 || mfDistrib.EffectivelySmooth()) return {};
        // https://www.pbr-book.org/4ed/Reflection_Models/Rough_Dielectric_BSDF#x0-RoughDielectricBSDF
        // 1. Sample Half Vector
        float cosTheta_o = CosTheta(wo), cosTheta_i = CosTheta(wi);
        // \frac{sin in}{sin out} > n -> Over critical angle -> no transmission, equivalent to IOR=1.0
        bool reflected = cosTheta_i * cosTheta_o > 0;
        float etap = 1;
        if (!reflected)
           etap = (cosTheta_o > 0) ? eta : 1 / eta;
        float3 wm = wi * etap + wo;
        if (cosTheta_i == 0 || cosTheta_o == 0 || LengthSquared(wm) == 0) return {};
        wm = FaceForward(normalize(wm), float3(0, 0, 1));
        // 2. Discard back-facing microfacets to either in/out directions
        if (dot(wm,wi) * cosTheta_i < 0 || dot(wm,wo) * cosTheta_o < 0)
           return 0;
        // 3. Determine reflectance
        float R = FrDielectric(dot(wo, wm), eta), T = 1 - R;
        if (reflected) {
            // 4. Eval rough reflection
            return SampledSpectrum(R * mfDistrib.D(wm) * mfDistrib.G(wo, wi) /
                                (4 * AbsCosTheta(wi) * AbsCosTheta(wo)));
        } else {
            // 4. Eval rough transmission
            // Same Jacobian as seen in previous transmission cases
            float denom = Sqr(dot(wi, wm) + dot(wo,wm) / etap) * abs(cosTheta_i * cosTheta_o);
            float ft = T * mfDistrib.D(wm) * mfDistrib.G(wo, wi) *
                                abs(dot(wi,wm) * dot(wo,wm) / denom);
            // 5. Adjust for non-symmetric transport
            if (transport == TransportMode::Radiance)
                ft /= Sqr(etap);
            return ft;
        }
   }
};
// https://pbr-book.org/4ed/Light_Transport_II_Volume_Rendering/Scattering_from_Layered_Materials
// **********************************************************************
// ATTENTION: This is basically copy-pasted...haven't read the Volume chapter yet.
//            And LayeredBxDF is *extremely* heavy to evaluate/sample due to lots of Random Walk.
//            A faster approximation would be using FresnelMix, which is single-scattering.
// **********************************************************************
public struct LayeredBxDF : IBxDF {
    IBxDF top;
    IBxDF bottom;
    float thickness;
    float g;
    SampledSpectrum albedo;
    int maxDepth, nSamples;
    bool twoSided;

    public __init(IBxDF top, IBxDF bottom, SampledSpectrum albedo, float thickness, float g, int maxDepth, int nSamples, bool twoSided) {
        this.top = top;
        this.bottom = bottom;
        this.albedo = albedo;
        this.thickness = thickness;
        this.g = g;
        this.maxDepth = maxDepth;
        this.nSamples = nSamples;
        this.twoSided = twoSided;
    }

    float Tr(float dz, float3 w) {
        if (abs(dz) <= 1e-6f) return 1.0f;
        return exp(-abs(dz / w.z));
    }

    public BxDFFlags Flags() {
        BxDFFlags topFlags = top.Flags();
        BxDFFlags bottomFlags = bottom.Flags();
        BxDFFlags flags = BxDFFlags::Reflection;

        if ((topFlags & BxDFFlags::Specular) != 0) flags = flags | BxDFFlags::Specular;

        if ((topFlags & BxDFFlags::Diffuse) != 0 || (bottomFlags & BxDFFlags::Diffuse) != 0 || dot(albedo,LUMINANCE_RGB) > 0)
            flags = flags | BxDFFlags::Diffuse;
        else if ((topFlags & BxDFFlags::Glossy) != 0 || (bottomFlags & BxDFFlags::Glossy) != 0)
            flags = flags |  BxDFFlags::Glossy;

        if (((topFlags & BxDFFlags::Transmission) != 0) && ((bottomFlags & BxDFFlags::Transmission) != 0))
            flags = flags |  BxDFFlags::Transmission;

        return flags;
    }

    public SampledSpectrum f(float3 wo, float3 wi, TransportMode mode) {
        float3 wo = wo;
        float3 wi = wi;
        if (twoSided && wo.z < 0) {
            wo = -wo;
            wi = -wi;
        }

        IBxDF enterInterface;
        if (twoSided || wo.z > 0)
            enterInterface = top;
        else
            enterInterface = bottom;
        bool enteredTop = (twoSided || wo.z > 0);

        IBxDF exitInterface, nonExitInterface;
        if (SameHemisphere(wo, wi) ^ enteredTop) {
            exitInterface = bottom;
            nonExitInterface = top;
        } else {
            exitInterface = top;
            nonExitInterface = bottom;
        }
        float exitZ = (SameHemisphere(wo, wi) ^ enteredTop) ? 0 : thickness;

        SampledSpectrum fSum = SampledSpectrum(0.0f);
        if (SameHemisphere(wo, wi))
            fSum = float(nSamples) * enterInterface.f(wo, wi, mode);

        PCG rng = PCG(float4(wi, dot(wi,wo)));

        for (int s = 0; s < nSamples; ++s) {
            // Sample transmission through entrance
            BSDFSample wos = enterInterface.Sample_f(wo, rng.sample(), rng.sample2D(), mode, BxDFReflTransFlags::Transmission);
            if (!wos.IsValid() || wos.wi.z == 0) continue;

            // Sample virtual light direction from wi
            BSDFSample wis = exitInterface.Sample_f(wi, rng.sample(), rng.sample2D(), !mode, BxDFReflTransFlags::Transmission);
            if (!wis.IsValid() || wis.wi.z == 0) continue;

            SampledSpectrum beta = wos.f * AbsCosTheta(wos.wi) / wos.pdf;
            float z = enteredTop ? thickness : 0;
            float3 w = wos.wi;
            HGPhaseFunction phase = HGPhaseFunction(g);

            for (int depth = 0; depth < maxDepth; ++depth) {
                // Russian Roulette
                if (depth > 3 && MaxComponent(beta) < 0.25f) {
                    float q = max(0.0f, 1.0f - MaxComponent(beta));
                    if (rng.sample() < q) break;
                    beta /= (1.0f - q);
                }

                if (MaxComponent(albedo) < EPS) {
                    // No scattering
                    z = (z == thickness) ? 0 : thickness;
                    beta *= Tr(thickness, w);
                } else {
                    const float sigma_t = 1.0f;
                    float dz = SampleExponential(rng.sample(), sigma_t / abs(w.z));
                    float zp = w.z > 0 ? (z + dz) : (z - dz);
                    if (zp > 0 && zp < thickness) {
                        // NEE via volume scattering
                        float wt = 1.0f;
                        if ((exitInterface.Flags() & BxDFFlags::Specular) == 0)
                            wt = PowerHeuristic(1, wis.pdf, 1, phase.PDF(-w, -wis.wi));

                        fSum += beta * albedo * phase.p(-w, -wis.wi) * wt * Tr(abs(zp - exitZ), wis.wi) * wis.f / wis.pdf;

                        // Sample Phase Function
                        PhaseFunctionSample ps = phase.Sample_p(-w, rng.sample2D());
                        if (ps.pdf == 0 || ps.wi.z == 0) break;
                        beta *= albedo * ps.p / ps.pdf;
                        w = ps.wi;
                        z = zp;

                        // Account for direct escape after scattering
                        if (((z < exitZ && w.z > 0) || (z > exitZ && w.z < 0)) && (exitInterface.Flags() & BxDFFlags::Specular) == 0) {
                            SampledSpectrum fExit = exitInterface.f(-w, wi, mode);
                            if (MaxComponent(fExit) > EPS) {
                                float exitPDF = exitInterface.PDF(-w, wi, mode, BxDFReflTransFlags::Transmission);
                                float wt2 = PowerHeuristic(1, ps.pdf, 1, exitPDF);
                                fSum += beta * Tr(abs(zp - exitZ), w) * fExit * wt2;
                            }
                        }
                        continue;
                    }
                    z = clamp(zp, 0.0f, thickness);
                }

                // Interface scattering
                if (z == exitZ) {
                    BSDFSample bs = exitInterface.Sample_f(-w, rng.sample(), rng.sample2D(), mode, BxDFReflTransFlags::Reflection);
                    if (!bs.IsValid() || bs.wi.z == 0) break;
                    beta *= bs.f * AbsCosTheta(bs.wi) / bs.pdf;
                    w = bs.wi;
                } else {
                    if ((nonExitInterface.Flags() & BxDFFlags::Specular) == 0) {
                        float wt = 1.0f;
                        if ((exitInterface.Flags() & BxDFFlags::Specular) == 0)
                            wt = PowerHeuristic(1, wis.pdf, 1, nonExitInterface.PDF(-w, -wis.wi, mode));
                        fSum += beta * nonExitInterface.f(-w, -wis.wi, mode) * AbsCosTheta(wis.wi) * wt * Tr(thickness, wis.wi) * wis.f / wis.pdf;
                    }

                    BSDFSample bs = nonExitInterface.Sample_f(-w, rng.sample(), rng.sample2D(), mode, BxDFReflTransFlags::Reflection);
                    if (!bs.IsValid() || bs.wi.z == 0) break;
                    beta *= bs.f * AbsCosTheta(bs.wi) / bs.pdf;
                    w = bs.wi;

                    if ((exitInterface.Flags() & BxDFFlags::Specular) == 0) {
                        SampledSpectrum fExit = exitInterface.f(-w, wi, mode);
                        if (MaxComponent(fExit) > EPS) {
                            float wt = 1.0f;
                            if ((nonExitInterface.Flags() & BxDFFlags::Specular) == 0) {
                                float exitPDF = exitInterface.PDF(-w, wi, mode, BxDFReflTransFlags::Transmission);
                                wt = PowerHeuristic(1, bs.pdf, 1, exitPDF);
                            }
                            fSum += beta * Tr(thickness, bs.wi) * fExit * wt;
                        }
                    }
                }
            }
        }
        return fSum / float(nSamples);
    }

    public BSDFSample Sample_f(float3 wo, float uc, float2 u, TransportMode mode, BxDFReflTransFlags sampleFlags = BxDFReflTransFlags::All) {
        bool flipWi = false;
        float3 wo = wo;
        if (twoSided && wo.z < 0) {
            wo = -wo;
            flipWi = true;
        }

        bool enteredTop = twoSided || wo.z > 0;
        IBxDF entryInterface;
        if (enteredTop)
            entryInterface = top;
        else
            entryInterface = bottom;

        BSDFSample bs = entryInterface.Sample_f(wo, uc, u, mode);
        if (!bs.IsValid() || bs.wi.z == 0) return BSDFSample();

        if (IsReflection(bs.flags)) {
            if (flipWi) bs.wi = -bs.wi;
            return bs;
        }

        float3 w = bs.wi;
        bool specularPath = IsSpecular(bs.flags);
        SampledSpectrum f = bs.f * AbsCosTheta(bs.wi);
        float pdf = bs.pdf;
        float z = enteredTop ? thickness : 0;
        HGPhaseFunction phase = HGPhaseFunction(g);
        PCG rng = PCG(float4(w, dot(w,wo)));

        for (int depth = 0; depth < maxDepth; ++depth) {
            // Russian Roulette
            float rrBeta = MaxComponent(f) / pdf;
            if (depth > 3 && rrBeta < 0.25f) {
                float q = max(0.0f, 1.0f - rrBeta);
                if (rng.sample() < q) return BSDFSample();
                pdf *= (1.0f - q);
            }

            if (MaxComponent(albedo) > EPS) {
                float dz = SampleExponential(rng.sample(), 1.0f / AbsCosTheta(w));
                float zp = w.z > 0 ? (z + dz) : (z - dz);
                if (zp > 0 && zp < thickness) {
                    PhaseFunctionSample ps = phase.Sample_p(-w, rng.sample2D());
                    if (ps.pdf == 0 || ps.wi.z == 0) return BSDFSample();
                    f *= albedo * ps.p;
                    pdf *= ps.pdf;
                    specularPath = false;
                    w = ps.wi;
                    z = zp;
                    continue;
                }
                z = clamp(zp, 0.0f, thickness);
            } else {
                z = (z == thickness) ? 0 : thickness;
                f *= Tr(thickness, w);
            }

            IBxDF currInterface;
            if (z == 0) currInterface = bottom;
            else currInterface = top;
            BSDFSample bs_int = currInterface.Sample_f(-w, rng.sample(), rng.sample2D(), mode);
            if (!bs_int.IsValid() || bs_int.wi.z == 0) return BSDFSample();

            f *= bs_int.f;
            pdf *= bs_int.pdf;
            specularPath = specularPath && IsSpecular(bs_int.flags);
            w = bs_int.wi;

            if (IsTransmission(bs_int.flags)) {
                BxDFFlags flags = SameHemisphere(wo, w) ? BxDFFlags::Reflection : BxDFFlags::Transmission;
                flags = flags | (specularPath ? BxDFFlags::Specular : BxDFFlags::Glossy);
                if (flipWi) w = -w;
                return BSDFSample(f, w, pdf, flags);
            }
            f *= AbsCosTheta(bs_int.wi);
        }
        return BSDFSample();
    }

    public float PDF(float3 wo, float3 wi, TransportMode mode, BxDFReflTransFlags flags) {
        float3 wo = wo;
        float3 wi = wi;
        if (twoSided && wo.z < 0) {
            wo = -wo;
            wi = -wi;
        }

        PCG rng = PCG(float4(wi, dot(wi,wo)));
        bool enteredTop = twoSided || wo.z > 0;
        float pdfSum = 0;

        if (SameHemisphere(wo, wi)) {
            pdfSum += float(nSamples) * (enteredTop ? top.PDF(wo, wi, mode, flags) : bottom.PDF(wo, wi, mode, flags));
        }

        for (int s = 0; s < nSamples; ++s) {
            if (SameHemisphere(wo, wi)) {
                // TRT estimate
                IBxDF rInterface;
                IBxDF tInterface;
                if (enteredTop) { rInterface = bottom; tInterface = top; }
                else { rInterface = top; tInterface = bottom; }
                BSDFSample wos = tInterface.Sample_f(wo, rng.sample(), rng.sample2D(), mode, BxDFReflTransFlags::Transmission);
                BSDFSample wis = tInterface.Sample_f(wi, rng.sample(), rng.sample2D(), !mode, BxDFReflTransFlags::Transmission);

                if (wos.IsValid() && wis.IsValid()) {
                    if ((tInterface.Flags() & BxDFFlags::Specular) != 0) {
                        pdfSum += rInterface.PDF(-wos.wi, -wis.wi, mode);
                    } else {
                        BSDFSample rs = rInterface.Sample_f(-wos.wi, rng.sample(), rng.sample2D(), mode);
                        if (rs.IsValid()) {
                            float rPDF = rInterface.PDF(-wos.wi, -wis.wi, mode);
                            pdfSum += PowerHeuristic(1, wis.pdf, 1, rPDF) * rPDF;
                            float tPDF = tInterface.PDF(-rs.wi, wi, mode);
                            pdfSum += PowerHeuristic(1, rs.pdf, 1, tPDF) * tPDF;
                        }
                    }
                }
            } else {
                // TT estimate
                IBxDF toInterface;
                IBxDF tiInterface;
                if (enteredTop) { toInterface = top; tiInterface = bottom; }
                else { toInterface = bottom; tiInterface = top; }
                BSDFSample wos = toInterface.Sample_f(wo, rng.sample(), rng.sample2D(), mode);
                if (!wos.IsValid() || IsReflection(wos.flags)) continue;
                BSDFSample wis = tiInterface.Sample_f(wi, rng.sample(), rng.sample2D(), !mode);
                if (!wis.IsValid() || IsReflection(wis.flags)) continue;

                if ((toInterface.Flags() & BxDFFlags::Specular) != 0) pdfSum += tiInterface.PDF(-wos.wi, wi, mode);
                else if ((tiInterface.Flags() & BxDFFlags::Specular) != 0) pdfSum += toInterface.PDF(wo, -wis.wi, mode);
                else pdfSum += (toInterface.PDF(wo, -wis.wi, mode) + tiInterface.PDF(-wos.wi, wi, mode)) * 0.5f;
            }
        }
        return lerp(Inv4Pi, pdfSum / float(nSamples), 0.9f);
    }
};
// Utilities
// These are not from PBRT, but from other references where implementing the glTF
// model is concerned.
// --
// Translate F0 to Complex Fresnel IOR terms
// "Artist Friendly Metallic Fresnel", Gulbrandsen 2014, https://jcgt.org/published/0003/04/03/paper.pdf
public void FresnelFromF0(float r /* baseColor */, float g /* specularTint */, out float n, out float k){
    r = clamp(0.01, 0.99, r);
    n = (1-r)/(1+r) * g + (1-g) * (1+sqrt(r))/(1-sqrt(r));
    k = sqrt( 1/(1-r) * (r * (n+1)*(n+1) - (n-1)*(n-1)));
}
public void FresnelFromF0(float3 r /* baseColor */, float3 g /* specularTint */, out float3 n, out float3 k){
    for (int i = 0; i < 3; i++)
        FresnelFromF0(r[i], g[i], n[i], k[i]);
}
// --
// vvv From https://github.com/nvpro-samples/nvpro_core2/tree/main/nvshaders/pbr_ggx_microfacet.h.slang
public float ClampedDot(float2 v1, float2 v2) { return clamp(dot(v1, v2), 0, 1.0); }
public float ClampedDot(float3 v1, float3 v2) { return clamp(dot(v1, v2), 0, 1.0); }
// Schlick Fresnel approximation
public float SchlickFresnel(float F0, float F90, float VdotH)
{
  return F0 + (F90 - F0) * pow(1.0F - VdotH, 5.0F);
}
// Schlick Fresnel approximation for vectors
public float3 SchlickFresnel(float3 F0, float3 F90, float VdotH)
{
  return F0 + (F90 - F0) * pow(1.0F - VdotH, 5.0F);
}
// Schlick Fresnel approximation for float
public float SchlickFresnel(float ior, float VdotH)
{
  // Calculate reflectance at normal incidence (R0)
  float R0 = pow((1.0F - ior) / (1.0F + ior), 2.0F);

  // Fresnel reflectance using Schlick's approximation
  return R0 + (1.0F - R0) * pow(1.0F - VdotH, 5.0F);
}
// Fresnel eval approximation of layered materials
// See https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html#fresnel
public float3 glTFFresnelMix(float VdotH, float3 bottom, float3 top, float ior = 1.5f){
    float F0 = pow((ior - 1) / (ior + 1), 2);
    float3 F = SchlickFresnel(F0, 1.0f, VdotH);
    return lerp(bottom, top, F);
}
// Fresnel sampling approximation of layered materials
float glTFGlossyProb(float NdotV, float metallic, float ior = 1.5f)
{
    float F0 = pow((ior - 1) / (ior + 1), 2); // IOR=1.5->0.04
    float F = SchlickFresnel(F0, 1.0f, NdotV);
    return lerp(F, 1.0F, metallic);
}
// Adapted from nvpro_core2's bsdfSampleSimple, bsdfEvalSimple
// Cheap LayeredBxDF alternative with glTF's metallic-roughness model
// See also Appendix B. https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html#material-structure
public BSDFSample glTFEval(float3 wo, float3 wi, float3 baseColor, float metallic, float roughness, float3 specularTint){
    if (wo.z <= 0 || wi.z <= 0) return BSDFSample(0); // Only mirror reflection.
    float3 wm = normalize(wo + wi);
    float NdotV = ClampedCosTheta(wo);
    float NdotL = ClampedCosTheta(wi);
    float NdotH = ClampedCosTheta(wm);
    float VdotH = ClampedDot(wo, wm);
    // We combine the metallic and specular lobes into a single glossy lobe.
    // The metallic weight is     metallic *    fresnel(f0 = baseColor)
    // The specular weight is (1-metallic) *    fresnel(f0 = c_min_reflectance)
    // The diffuse weight is  (1-metallic) * (1-fresnel(f0 = c_min_reflectance)) * baseColor
    float c_min_reflectance = 0.04f;
    float3 f0 = lerp(float3(c_min_reflectance), baseColor, metallic);
    float3 fGlossy = SchlickFresnel(f0, 1.0f, VdotH);
    float3 fDiffuse = SchlickFresnel(1.0f - c_min_reflectance, 0.0f, VdotH) * (1.0f - metallic);
    // Draw from microfacet distribution
    // We don't use ConductorBxDF/DielectricBxDF as-is since our F0 is based on RGB values.
    // Usage of FresnelFromF0 should be investigated
    float alpha = roughness * roughness;
    float diffusePDF = InvPi * NdotL;
    SampledSpectrum diffuseBSDF = baseColor * fDiffuse * InvPi;
    float specularPDF;
    SampledSpectrum specularBSDF;
    TrowbridgeReitzDistribution mfDistrib = TrowbridgeReitzDistribution(alpha, alpha);
    if (mfDistrib.EffectivelySmooth()){
        // PDF is dirac delta, which for eval is impossible to represent
        specularPDF = 0.0f;
        specularBSDF = 0.0f;
    } else {
        specularPDF = mfDistrib.PDF(wo, wm) / (4 * VdotH);
        specularBSDF = mfDistrib.D(wm) * fGlossy * mfDistrib.G(wo, wi) / (4 * NdotV * NdotL);
    }
    return BSDFSample(specularBSDF + diffuseBSDF,
                      wi,
                      lerp(diffusePDF, specularPDF, glTFGlossyProb(NdotV, metallic)),
                      BxDFFlags::GlossyReflection);
}
public BSDFSample glTFSample(inout PCG rng, float3 wo, float3 baseColor, float metallic, float roughness, float3 specularTint)
{
    float c_min_reflectance = 0.04f;
    float3 f0 = lerp(float3(c_min_reflectance), baseColor, metallic);
    float3 wi;
    float2 u = rng.sample2D();
    float alpha = roughness * roughness;
    TrowbridgeReitzDistribution mfDistrib = TrowbridgeReitzDistribution(alpha, alpha);
    if (rng.sample() <= glTFGlossyProb(ClampedCosTheta(wo), metallic)) {
        // Sample Specular
        if (mfDistrib.EffectivelySmooth()) {
            // Dirac delta case
            wi = float3(-wo.x, -wo.y, wo.z);
            return BSDFSample(f0, wi, 1.0f, BxDFFlags::SpecularReflection);
        } else {
            float3 wm = mfDistrib.Sample_wm(wo, u);
            wi = Reflect(wo, wm);
        }
    } else {
        // Sample Diffuse
        wi = SampleCosineHemisphere(u);
    }
    wi = FaceForward(wi, float3(0,0,1));
    return glTFEval(wo, wi, baseColor, metallic, roughness, specularTint);
}