import IBSDF;
import IBSSRDF;
import IBxDF;
import IMath;

public interface IMaterial {
    public IBSDF GetSurface();
    public IBSSRDF GetSubsurface();
    public float ProbSampleSurface();
    public float ProbSampleSubsurface();
    public float3 Le(float3 wo);
};

public struct PrincipledMaterial : IMaterial {
    PrincipledBSDF surface;
    DisneyBurleyBSSRDF subsurface;

    float clearcoatFactor;
    float clearcoatRoughness;
    // Affected by clearcoat, emission categorically should belong to the bottom
    // layer.
    // For BSDF/(future)Light sampling where they'd get added, Le() evaluates the modulated
    // exitance radiance.
    float3 emission;
    uint32_t ggxLutEIndex;

    float pSurface;
    float pSubsurface;

    public __init(
        float3 baseColor,
        float metallic,
        float roughness,
        float ior,
        float specularFactor = 1.0f,
        float transmission = 0.0f,
        bool energyCompensation = true,
        float3 wo = float3(0.0f, 0.0f, 1.0f),
        float subsurfaceWeight = 0.0f,
        float3 subsurfaceColor = float3(1.0f),
        float3 subsurfaceRadius = float3(1.0f, 0.2f, 0.1f),
        float subsurfaceScale = 0.05f,
        float anisotropyStrength = 0.0f,
        float3 specularColorFactor = float3(1.0f),
        uint32_t ggxLutEIndex = ~0u,
        float sheenWeight = 0.0f,
        float sheenRoughness = 0.0f,
        float3 sheenTint = float3(1.0f),
        uint32_t sheenLtcIndex = ~0u,
        float clearcoatFactor = 0.0f,
        float clearcoatRoughness = 0.0f,
        float3 emission = float3(0.0f))
    {
        bool hasSubsurfaceProfile = MaxComponent(subsurfaceRadius * subsurfaceScale) > EPS &&
                                    MaxComponent(baseColor * subsurfaceColor) > EPS;
        float activeSubsurface = hasSubsurfaceProfile ? clamp(subsurfaceWeight, 0.0f, 1.0f) : 0.0f;
        float diffuseWeight = 1.0f - activeSubsurface;

        roughness = max(8e-3f, roughness);
        ior = max(1.0f + 5e-3f, ior);

        // Keep the subsurface closure in the same energy family as diffuse.
        // Otherwise sampling probability cancels this factor and SSS becomes
        // too strong relative to the white specular component.
        float3 dielF0, dielF90;
        CalcGlossyFresnel(ior, specularFactor, specularColorFactor, dielF0, dielF90);
        float sampleGlossy = MaxComponent(reflectance(dielF0, dielF90,
            SampleGGXLutE(ggxLutEIndex, AbsCosTheta(wo), roughness)));
        float rawCoat = clamp(MaxComponent(CoatReflectance(clearcoatFactor, clearcoatRoughness, wo, ggxLutEIndex)), 0.0f, 1.0f);
        float rawUnderCoat = max(1.0f - rawCoat, 0.0f);
        SheenBSDF sampleSheen = SheenBSDF(sheenWeight, sheenRoughness, sheenTint, sheenLtcIndex, wo);
        float rawSheen = rawUnderCoat * (sampleSheen.IsValid() ? clamp(MaxComponent(sampleSheen.Reflectance()), 0.0f, 1.0f) : 0.0f);
        float rawBase = max(rawUnderCoat - rawSheen, 0.0f);
        float rawDiffuseFamily = rawBase * (1.0f - metallic) * (1.0f - sampleGlossy) * (1.0f - transmission);
        float rawMetal = rawBase * metallic;
        float rawDielSpec = rawBase * (1.0f - metallic) * sampleGlossy;
        float rawTrans = rawBase * (1.0f - metallic) * (1.0f - sampleGlossy) * transmission;
        float rawSubsurface = rawDiffuseFamily * activeSubsurface;
        float eventWeightSum = rawDiffuseFamily + rawTrans + rawDielSpec + rawMetal + rawSheen + rawCoat;

        this.surface = PrincipledBSDF(
            baseColor, metallic, roughness, ior, specularFactor, transmission,
            energyCompensation, wo, diffuseWeight, anisotropyStrength, specularColorFactor, ggxLutEIndex,
            sheenWeight, sheenRoughness, sheenTint, sheenLtcIndex,
            clearcoatFactor, clearcoatRoughness);
        this.subsurface = DisneyBurleyBSSRDF(
            baseColor * subsurfaceColor * activeSubsurface * rawDiffuseFamily,
            subsurfaceRadius * subsurfaceScale);
        this.emission = max(float3(0.0f), emission);
        this.clearcoatFactor = clearcoatFactor;
        this.clearcoatRoughness = clearcoatRoughness;
        this.ggxLutEIndex = ggxLutEIndex;

        // Weigh surface/subsurface events by energy
        this.pSubsurface = clamp(eventWeightSum > 0.0f ? rawSubsurface / eventWeightSum : 0.0f, 0.0f, 1.0f);
        this.pSurface = 1.0f - this.pSubsurface;
    }

    public IBSDF GetSurface() {
        return surface;
    }

    public IBSSRDF GetSubsurface() {
        return subsurface;
    }

    public float ProbSampleSurface() {
        return pSurface;
    }

    public float ProbSampleSubsurface() {
        return pSubsurface;
    }

    public float3 Le(float3 wo) {
        return emission * CoatEscape(clearcoatFactor, clearcoatRoughness, wo, ggxLutEIndex);
    }
};

// Chiang 16
public struct PrincipledHairMaterial : IMaterial {
    HairBSDF surface;
    StubBSSRDF subsurface;
    float3 emission;

    public __init(
        float3 baseColor,
        float betaM,
        float betaN,
        float h = 0.0f,
        float eta = 1.55f,
        float alpha = 2.0f,
        float3 emission = float3(0.0f))
    {
        this.surface = HairBSDF(baseColor, betaM, betaN, h, eta, alpha);
        this.subsurface = StubBSSRDF();
        this.emission = max(float3(0.0f), emission);
    }

    public IBSDF GetSurface() {
        return surface;
    }

    public IBSSRDF GetSubsurface() {
        return subsurface;
    }

    public float ProbSampleSurface() {
        return 1.0f;
    }

    public float ProbSampleSubsurface() {
        return 0.0f;
    }

    public float3 Le(float3 wo) {
        return emission;
    }
};