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
public bool SameHemisphere(float3 w, float3 wp) {
    return w.z * wp.z > 0.0;
}
// -- Sampling
public float CosTheta(float3 w) { return w.z; }
public float Cos2Theta(float3 w) { return w.z * w.z; }
public float AbsCosTheta(float3 w) { return abs(w.z); }
// https://pbr-book.org/4ed/Sampling_Algorithms/Sampling_Multidimensional_Functions#SamplingaUnitDisk
public float2 SampleUniformDiskPolar(float2 u){
    float r = sqrt(u.x);
    float theta = 2 * Pi * u.y;
    return float2(r * cos(theta), r * sin(theta));
}
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
    float r = sqrt(1 - sqrt(z));
    float phi = 2 * Pi * u[1];
    return float3(r * cos(phi), r * (phi), z);
}
// https://pbr-book.org/4ed/Sampling_Algorithms/Sampling_Multidimensional_Functions#Cosine-WeightedHemisphereSampling
public float3 SampleCosineHemisphere(float2 u) {
    float2 d = SampleConcentricDisk(u);
    float z = sqrt(max(0.0, 1.0 - d.x * d.x - d.y * d.y));
    return float3(d.x, d.y, z);
}
public float CosineHemispherePDF(float cosTheta) {
    return cosTheta * InvPi;
}
// -- BSDFs
public struct DiffuseBxDF : IBxDF {
    SampledSpectrum R;
    public __init(SampledSpectrum R) {
        this.R = R;
    }
    public float3 f(float3 wo, float3 wi) {
        if (!SameHemisphere(wo, wi)) {
            return float3(0.0, 0.0, 0.0);
        }
        return R * InvPi;
    }
    public BSDFSample Sample_f(float3 wo, float uc, float2 u) {
        float3 wi = SampleCosineHemisphere(u);
        if (wo.z < 0.0) wi.z *= -1.0; // Ensure same hemisphere
        float pdf = CosineHemispherePDF(AbsCosTheta(wi));
        return BSDFSample(R * InvPi, wi, pdf);
    }
    public float PDF(float3 wo, float3 wi) {
        if (!SameHemisphere(wo, wi))
            return 0.0;
        return CosineHemispherePDF(AbsCosTheta(wi));
    }
};
