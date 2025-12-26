import IBRDF;
import IMath;
// -- glTF BSDF
// Energy compensation LUTs. See https://mos9527.com/posts/foundation/pt-6-path-tracing-adventures/#ee_avg-%E9%A2%84%E7%A7%AF%E5%88%86
[[vk_binding(0,1)]] SamplerState lutSampler;
[[vk_binding(1,1)]] Texture2D<float2> ggxLutE;
[[vk_binding(2,1)]] Texture2D<float> ggxLutEavg;
// Fresnel eval approximation of layered materials
// See https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html#fresnel
public float3 glTFFresnelMix(float cosTheta, float3 bottom, float3 top, float ior = 1.5f){
    float F0 = pow((ior - 1) / (ior + 1), 2);
    float3 F = SchlickFresnel(F0, 1.0f, cosTheta);
    return lerp(bottom, top, F);
}
// Cheap, single-scattering LayeredBxDF alternative with glTF's metallic-roughness model
// See also Appendix B. https://registry.khronos.org/glTF/specs/2.0/glTF-2.0.html#material-structure
public struct glTFBSDF : IBxDF {
    float3 baseColor;
    float metallic;
    float roughness;
    TrowbridgeReitzDistribution mfDistrib;

    bool energyCompensation;
    public __init(float3 baseColor, float metallic, float roughness, bool energyCompensation = true) {
        this.baseColor = baseColor;
        this.metallic = metallic;
        this.roughness = roughness;
        float alpha = roughness * roughness;
        this.mfDistrib = TrowbridgeReitzDistribution(alpha, alpha);
        this.energyCompensation = energyCompensation;
    }
    public BxDFFlags Flags() {
        return BxDFFlags::Reflection | BxDFFlags::Glossy;
    }
    public float NEEGlossyProb(float3 wo) {
        // float F0 = pow((ior - 1) / (ior + 1), 2); // IOR=1.5->0.04
        float probGlossy = SchlickFresnel(0.04, 1.0f, ClampedCosTheta(wo));
        return lerp(probGlossy, 1.0f, metallic); // Fully metallic means there's no diffuse lobe
    }
    public BSDFSample Sample_f(float3 wo, float uc, float2 u, TransportMode, BxDFReflTransFlags) {
        float c_min_reflectance = 0.04f;
        // Mixed Metallic/Dielectric Fresnel F0
        const float3 F0 = lerp(float3(c_min_reflectance), baseColor, metallic);
        const float3 F90 = 1.0f; // As per glTF spec
        // Multi-scatter compensation
        // https://mos9527.com/posts/foundation/pt-6-path-tracing-adventures/#%E8%83%BD%E9%87%8F%E5%AE%88%E6%81%92%E6%94%B9%E8%BF%9B
        const float mu = AbsCosTheta(wo);
        const float2 EEp = ggxLutE.SampleLevel(lutSampler, float2(mu, roughness), 0);
        const float3 Epp = F0 * EEp.x + (F90 - F0) * EEp.y;
        float probGlossy = NEEGlossyProb(wo);
        if (uc < probGlossy) {
            // Sample Glossy
            if (mfDistrib.EffectivelySmooth()) {
                // Dirac delta case
                float3 wi = float3(-wo.x, -wo.y, wo.z); // = wr
                wi = FaceForward(wi, float3(0,0,1));
                float3 wm = normalize(wi + wo);

                float3 Fss = SchlickFresnel(F0, 1.0f, AbsDot(wo, wm));
                float3 Fms = F0 * (1/Epp - 1) * Fss;
                if (!energyCompensation)
                    Fms = 0;
                float3 f = (Fss+Fms) / AbsCosTheta(wi);
                // Sampled PDF would be delta, but we represent them as 1s w/o weighting
                // With NEE this is what you get:
                float pdf = 1.0f * probGlossy;
                return BSDFSample(f, wi, pdf, BxDFFlags::SpecularReflection);
            } else {
                float3 wm = mfDistrib.Sample_wm(wo, u);
                float3 wi = Reflect(wo, wm);
                if (!SameHemisphere(wo, wi))
                    return BSDFSample(); // Absorption

                float3 Fss = SchlickFresnel(F0, 1.0f, AbsDot(wo, wm));
                float3 Fms = F0 * (1/Epp - 1) * Fss;
                if (!energyCompensation)
                    Fms = 0;
                float3 f = (Fms + Fss) * mfDistrib.D(wm) * mfDistrib.G(wo, wi) / (4 * AbsCosTheta(wi) * AbsCosTheta(wo));

                float pdf = probGlossy * mfDistrib.PDF(wo, wm) / (4 * AbsDot(wo, wm));
                return BSDFSample(f, wi, pdf, BxDFFlags::GlossyReflection);
            }
        } else {
            // Sample Diffuse
            float3 wi = SampleCosineHemisphere(u);
            wi = FaceForward(wi, float3(0,0,1));
            float3 wm = normalize(wo + wi);

            float3 cdiff = baseColor * (1.0f - metallic);
            float3 f = cdiff * InvPi;
            if (energyCompensation)
                f *= 1 - Epp;

            float pdf = (1 - probGlossy) * CosineHemispherePDF(ClampedCosTheta(wi));
            return BSDFSample(f, wi, pdf, BxDFFlags::DiffuseReflection);
        }
    }
    public SampledSpectrum f(float3 wo, float3 wi, TransportMode) {
        if (wo.z <= 0 || wi.z <= 0) return 0; // Only mirror reflection.
        float3 wm = normalize(wo + wi);
        float c_min_reflectance = 0.04f;
        float3 F0 = lerp(float3(c_min_reflectance), baseColor, metallic);
        // Same fresnel terms as before.
        float3 fGlossy = SchlickFresnel(F0, 1.0f, ClampedDot(wo, wm));
        float3 fDiffuse = (1.0f - SchlickFresnel(c_min_reflectance, 1.0f, ClampedDot(wo, wm))) ;
        // The two lobes - remember NEE only affects the PDF in sampling, not the eval.
        SampledSpectrum diffuseBSDF = baseColor * (1.0f - metallic) * InvPi * fDiffuse;
        SampledSpectrum specularBSDF = mfDistrib.D(wm) * fGlossy * mfDistrib.G(wo, wi) / (4 * AbsCosTheta(wi) * AbsCosTheta(wo));
        // NEE for the combined lobe
        if (mfDistrib.EffectivelySmooth()){
            // PDF is dirac delta, which for eval is impossible to represent
            // This is the specular case again, so eval there has zero contribution
            return diffuseBSDF;
        } else {
            return diffuseBSDF + specularBSDF;
        }
    }
    public float PDF(float3 wo, float3 wi, TransportMode, BxDFReflTransFlags) {
        float3 wm = normalize(wo + wi);
        float diffusePDF = CosineHemispherePDF(ClampedCosTheta(wi));
        float specularPDF = mfDistrib.PDF(wo, wm) / (4 * ClampedDot(wo, wm));
        if (mfDistrib.EffectivelySmooth())
            specularPDF = 0.0f;
        float probGlossy = NEEGlossyProb(wo);
        return diffusePDF * (1 - probGlossy) + specularPDF * probGlossy;
    }
};
