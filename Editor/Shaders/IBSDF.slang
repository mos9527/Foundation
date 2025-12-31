import IBxDF;
import IMath;

[[vk_binding(0,1)]] SamplerState lutSampler;
[[vk_binding(1,1)]] Texture2D<float2> ggxLutE;

public float2 SampleGGXLutE(float mu, float roughness) {
    return ggxLutE.SampleLevel(lutSampler, float2(mu, roughness), 0);
}
// -- OSL stdlib routines
import glsl;
// Directional Albedo
public float reflectance(float F0, float F90, float2 lutE) {
    return F0 * lutE.x + (F90 - F0) * lutE.y;
}
public float3 reflectance(float3 F0, float3 F90, float2 lutE) {
    return F0 * lutE.x + (F90 - F0) * lutE.y;
}
// BSDF Layering function
// https://open-shading-language.readthedocs.io/en/v1.13.12.0/stdlib.html#layering-and-signaling-closures
public float layer(float base, float top, float topReflectance /* from reflectance() */) {
    return top + base * (1.0f - topReflectance);
}
public float3 layer(float3 base, float3 top, float3 topReflectance /* from reflectance() */) {
    return top + base * (1.0f - topReflectance);
}

// Inspired by Blender's OSL implementation
//  https://projects.blender.org/blender/blender/src/commit/96d715c643888c78e5dbaa8bd3c3c79ce599c0a3/intern/cycles/kernel/osl/shaders/node_principled_bsdf.osl#L15
public struct PrincipledBSDF /* glTF Core Spec ver */ : IBxDF {
    const static float dielF0 = 0.04f;
    const static float dielF90 = 1.0f;
    // const static float3 metalF0 -> baseColor
    const static float metalF90 = 1.0f;

    float3 baseColor;
    float metallic;
    float roughness;
    bool energyCompensation;

    TrowbridgeReitzDistribution mfDistrib;

    public __init(float3 baseColor, float metallic, float roughness, bool energyCompensation = true) {
        this.baseColor = baseColor;
        this.metallic = metallic;

        // Keep it simple - we don't need to deal with EffectivelySmooth this way
        roughness = max(EPS, roughness);

        this.roughness = roughness;
        this.energyCompensation = energyCompensation;

        float alpha = roughness * roughness;
        this.mfDistrib = TrowbridgeReitzDistribution(alpha, alpha);
    }

    public BxDFFlags Flags() {
        return BxDFFlags::Reflection | BxDFFlags::Glossy;
    }

    private float3 TorranceSparrowPreserveEnergy(float3 wo, float3 wi, float3 F0, float3 F90, float2 lutE) {
        float3 wm = normalize(wo + wi);
        float3 Fss = SchlickFresnel(F0, F90, AbsDot(wo, wm));
        float3 Fms = 0.0f;
        if (energyCompensation) {
            float3 Epp = max(F0 * lutE.x + (F90 - F0) * lutE.y, EPS);
            Fms = F0 * (1.0f / Epp - 1.0f) * Fss;
        }
        return (Fss + Fms) * mfDistrib.D(wm) * mfDistrib.G(wo, wi) / (4.0f * AbsCosTheta(wo) * AbsCosTheta(wi));
    }

    public SampledSpectrum f(float3 wo, float3 wi, TransportMode mode) {
        if (wo.z <= 0 || wi.z <= 0) return 0; // Reflection only
        float2 lutE = SampleGGXLutE(AbsCosTheta(wo), roughness);

        // Dielectric top/bottom
        float3 diffuseBSDF = baseColor * InvPi;
        float3 dielBSDF = TorranceSparrowPreserveEnergy(wo, wi, dielF0, dielF90, lutE);
        // Base Layer
        float3 bsdf = layer(diffuseBSDF, dielBSDF, reflectance(dielF0, dielF90, lutE));
        // Metal
        // There's no diffuse lobe anymore (completely absorbed!)
        // Blender has a F82 tint model for modeling F0, but for convenience's sake
        // (since glTF never does that) we'll use baseColor for that.
        float3 metalBSDF = TorranceSparrowPreserveEnergy(wo, wi, baseColor, metalF90, lutE);

        bsdf = mix(bsdf, metalBSDF, metallic);
        return bsdf;
    }


    public float PDF(float3 wo, float3 wi, TransportMode mode, BxDFReflTransFlags flags) {
        float3 wm = normalize(wo + wi);

        float sampleGlossy = reflectance(dielF0, dielF90, SampleGGXLutE(AbsCosTheta(wo), roughness));

        float glossyPDF =  mfDistrib.PDF(wo, wm) / (4.0f * AbsDot(wo, wm));
        // Dielectric top/bottom
        float dielPDF = glossyPDF;
        float diffusePDF = CosineHemispherePDF(AbsCosTheta(wi));

        // Base Layer (MIS)
        // https://pbr-book.org/4ed/Monte_Carlo_Integration/Improving_Efficiency#MultipleImportanceSampling
        // ---
        // Two sampling methods (techniques) can arrive here, with p(f) of glossyPDF and diffusePDF
        // with probabilities of sampleGlossy and (1 - sampleGlossy) respectively.
        // Assuming we'd use Balance Heuristic, the single sample model would be:
        //  Fs = fGlossy/pdfGlossy * (1/sampleGlossy) * BalanceHeuristic(1, pdfGlossy * sampleGlossy, 1, pdfDiffuse * (1 - sampleGlossy))
        // and for diffuse
        //  Fd = fDiffuse/pdfDiffuse * (1/(1 - sampleGlossy)) * BalanceHeuristic(1, pdfDiffuse * (1-sampleGlossy), 1, pdfGlossy * sampleGlossy)
        // ...Take the glossy path for example and simplify:
        //  Fs = fGlossy/pdfGlossy * (1/sampleGlossy) * ((pdfGlossy * sampleGlossy) / (pdfGlossy * sampleGlossy + pdfDiffuse * (1 - sampleGlossy)))
        //     = fGlossy / (pdfGlossy * sampleGlossy + pdfDiffuse * (1 - sampleGlossy))
        //  Fd = ...
        //     = fDiffuse / (pdfDiffuse * (1 - sampleGlossy) + pdfGlossy * sampleGlossy)
        // Denominator is the same! Thus the final PDF is simply:
        // PDF = pdfGlossy * sampleGlossy + pdfDiffuse * (1 - sampleGlossy)
        float pdf = diffusePDF * (1.0f - sampleGlossy) + dielPDF * sampleGlossy;

        // Metal (Linearly mixed)
        float metal = glossyPDF;
        pdf = mix(pdf, metal, metallic);
        return pdf;
    }

    public BSDFSample Sample_f(float3 wo, float uc, float2 u, TransportMode mode, BxDFReflTransFlags flags) {
        float3 wi;
        BxDFFlags sampledFlag;

        float sampleGlossy = reflectance(dielF0, dielF90, SampleGGXLutE(AbsCosTheta(wo), roughness));

        bool isGlossy = Select(uc, sampleGlossy);
        bool isMetal = Select(uc, metallic);

        if (isGlossy | isMetal) {
            // Glossy (dielectric/metal) sample
            float3 wm = mfDistrib.Sample_wm(wo, u);
            wi = Reflect(wo, wm);
            sampledFlag = BxDFFlags::GlossyReflection;
        } else {
            // Diffuse Sample
            wi = SampleCosineHemisphere(u);
            wi = FaceForward(wi, float3(0,0,1));
            sampledFlag = BxDFFlags::DiffuseReflection;
        }
        if (!SameHemisphere(wo, wi)) return BSDFSample();
        SampledSpectrum val = this.f(wo, wi, mode);
        float pdf = this.PDF(wo, wi, mode, flags);
        return BSDFSample(val, wi, pdf, sampledFlag);
    }
};
