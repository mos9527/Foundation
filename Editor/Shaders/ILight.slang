/**
* PBRTv4-style interface for physically modeled light sources
*/
import IMath;
import IBxDF;
public enum class LightType {
    DeltaPosition,  // Point light
    DeltaDirection, // Directional light
    Area,           // Surface light with a geometric shape
    Infinite        // Ambient light for rays that left the scene
};
bool IsDeltaLight(LightType type) {
    return (type == LightType::DeltaPosition ||
            type == LightType::DeltaDirection);
}
public struct LightSampleContext {
    public float3 pi; // Surface interaction point
    public float3 n;  // Geometric normal at pi
    public float3 ns; // Shading normal at pi
    public __init(float3 pi, float3 n, float3 ns) {
        this.pi = pi;
        this.n = n;
        this.ns = ns;
    }
    public float3 p() { return pi; }
};
public struct LightLiSample {
    public SampledSpectrum Li; // Light leaving the source towards pi
    public float3 wi;          // Direction from pi to light source
    public float pdf;          // PDF for this sample
    public bool isDelta;
    public __init(SampledSpectrum Li = 0, float3 wi = 0, float pdf = 0, bool isDelta = false) {
        this.Li = Li;
        this.wi = wi;
        this.pdf = pdf;
        this.isDelta = isDelta;
    }
};
// The light interface
// There's _no_ spectrum sampling here - all spectra are fully sampled RGB for simplicity
public interface ILight {
    // Type of this light source
    public LightType Type();
    // Area light interaction - only defined for lights with geo (Area)
    public SampledSpectrum L(float3 p, float3 n, float3 uv, float3 w);
    // Infinite/environment light interaction - only defined for ambient lights (Infinite)
    public SampledSpectrum Le(float3 w);
    // PDF for Le - only defined for ambient lights (Infinite)
    public void PDF_Le(LightSampleContext ctx, float3 wi, out float pdfPos, out float pdfDir);
    // PDF for Li sampling - only defined for ones that make sense to be sampled
    public float PDF_Li(LightSampleContext ctx, float3 wi, bool allowIncompletePDF = false);
    // Sampled light
    public LightLiSample Sample_Li(LightSampleContext ctx, float2 u);
};
// -- Lights

// Module-level env map resources — bound by the integrator.
// When no env map is loaded a 1×1 black fallback is bound so sampling is always safe.
public Texture2D<float4> envMapTexture;
public SamplerState        envMapSampler;

// Direction → equirectangular UV (azimuth-elevation mapping)
// φ = atan2(d.z, d.x) ∈ [-π,π],  θ = acos(d.y) ∈ [0,π]
float2 EnvMapDirToUV(float3 d) {
    float phi   = atan2(d.z, d.x);
    float theta = acos(clamp(d.y, -1, 1));
    return float2(0.5f + phi * (0.5f * InvPi), theta * InvPi);
}

float3 SampleEnvMapTexture(float3 dir) {
    return envMapTexture.SampleLevel(envMapSampler, EnvMapDirToUV(dir), 0).xyz;
}

// https://pbr-book.org/4ed/Light_Sources/Infinite_Area_Lights
// Unified infinite light: when hasEnvMap is true, radiance comes from the
// equirectangular env map texture; otherwise it falls back to a constant color.
// Sampling is always uniform-sphere (importance sampling the map is a TODO).
public struct InfiniteLight : ILight {
    SampledSpectrum fallbackColor; // constant Lemit when no env map
    float scale;
    bool  hasEnvMap;

    public __init(SampledSpectrum fallbackColor, float scale, bool hasEnvMap = false) {
        this.fallbackColor = fallbackColor;
        this.scale         = scale;
        this.hasEnvMap     = hasEnvMap;
    }

    public LightType Type() {
        return LightType::Infinite;
    }
    // Not defined for infinite lights
    public SampledSpectrum L(float3 p, float3 n, float3 uv, float3 w) {
        return {};
    }
    public SampledSpectrum Le(float3 w) {
        if (hasEnvMap)
            return SampledSpectrum(SampleEnvMapTexture(w)) * scale;
        return fallbackColor * scale;
    }
    public void PDF_Le(LightSampleContext, float3, out float pdfPos, out float pdfDir) {
        pdfPos = 0.0f;
        pdfDir = UniformSpherePDF();
    }
    public LightLiSample Sample_Li(LightSampleContext ctx, float2 u) {
        float3 wi = SampleUniformSphere(u);
        return LightLiSample(Le(wi), wi, UniformSpherePDF(), false);
    }
    public float PDF_Li(LightSampleContext ctx, float3 w, bool allowIncompletePDF) {
        return UniformSpherePDF();
    }
};
// https://pbr-book.org/4ed/Light_Sources/Distant_Lights#DistantLight
public struct DistantLight : ILight {
    SampledSpectrum Lemit;
    float3 direction;
    public __init(SampledSpectrum Lemit, float3 direction){
        this.Lemit = Lemit;
        this.direction = normalize(direction);
    }
    public LightType Type() {
        return LightType::DeltaDirection;
    }
    // Not defined
    public SampledSpectrum L(float3 p, float3 n, float3 uv, float3 w) {
        return {};
    }
    // Not defined
    public SampledSpectrum Le(float3 w) {
        return {};
    }
    // Not defined
    public void PDF_Le(LightSampleContext, float3, out float pdfPos, out float pdfDir){
        pdfPos = 0.0f;
        pdfDir = 0.0f; // Dirac
    }
    // Dirac
    public float PDF_Li(LightSampleContext, float3, bool allowIncompletePDF) {
        return 0.0f;
    }
    public LightLiSample Sample_Li(LightSampleContext ctx, float2 u) {
        return LightLiSample(Lemit, -direction, 1.0f, true);
    }
};

public struct PointLight : ILight {
    SampledSpectrum I;
    float3 position;

    public __init(SampledSpectrum I, float3 position) {
        this.I = I;
        this.position = position;
    }

    public LightType Type() {
        return LightType::DeltaPosition;
    }

    public SampledSpectrum L(float3 p, float3 n, float3 uv, float3 w) {
        return {};
    }

    public SampledSpectrum Le(float3 w) {
        return {};
    }

    public void PDF_Le(LightSampleContext ctx, float3 wi, out float pdfPos, out float pdfDir) {
        pdfPos = 0.0f;
        pdfDir = 0.0f;
    }

    public float PDF_Li(LightSampleContext ctx, float3 wi, bool allowIncompletePDF) {
        return 0.0f;
    }

    public LightLiSample Sample_Li(LightSampleContext ctx, float2 u) {
        float3 wi = position - ctx.p();
        float dist2 = dot(wi, wi);
        if (dist2 == 0.0f) return LightLiSample();
        
        float dist = sqrt(dist2);
        wi /= dist;
        
        SampledSpectrum Li = I / dist2;
        return LightLiSample(Li, wi, 1.0f, true);
    }
};

public struct SpotLight : ILight {
    SampledSpectrum I;
    float3 position;
    float3 direction;
    float cosTotalWidth;
    float cosFalloffStart;

    public __init(SampledSpectrum I, float3 position, float3 direction, float totalWidth, float falloffStart) {
        this.I = I;
        this.position = position;
        this.direction = normalize(direction);
        this.cosTotalWidth = cos(totalWidth);
        this.cosFalloffStart = cos(falloffStart);
    }

    public LightType Type() {
        return LightType::DeltaPosition;
    }

    public SampledSpectrum L(float3 p, float3 n, float3 uv, float3 w) {
        return {};
    }

    public SampledSpectrum Le(float3 w) {
        return {};
    }

    public void PDF_Le(LightSampleContext ctx, float3 wi, out float pdfPos, out float pdfDir) {
        pdfPos = 0.0f;
        pdfDir = 0.0f;
    }

    public float PDF_Li(LightSampleContext ctx, float3 wi, bool allowIncompletePDF) {
        return 0.0f;
    }

    float Falloff(float3 w) {
        float cosTheta = dot(direction, w);
        if (cosTheta < cosTotalWidth) return 0.0f;
        if (cosTheta > cosFalloffStart) return 1.0f;
        float delta = (cosTheta - cosTotalWidth) / (cosFalloffStart - cosTotalWidth);
        return (delta * delta) * (delta * delta);
    }

    public LightLiSample Sample_Li(LightSampleContext ctx, float2 u) {
        float3 wi = position - ctx.p();
        float dist2 = dot(wi, wi);
        if (dist2 == 0.0f) return LightLiSample();
        
        float dist = sqrt(dist2);
        wi /= dist;
        
        float falloff = Falloff(-wi);
        if (falloff == 0.0f) return LightLiSample();

        SampledSpectrum Li = I * falloff / dist2;
        return LightLiSample(Li, wi, 1.0f, true);
    }
};

// https://pbr-book.org/4ed/Light_Sources/Area_Lights
public struct DiskLight : ILight {
    SampledSpectrum Lemit;
    float3 position;
    float3 normal;
    float3 dpdu;
    float3 dpdv;
    float radius;
    float area;
    bool twoSided;

    public __init(SampledSpectrum Lemit, float3 position, float3 normal, float3 dpdu, float radius, bool twoSided = false) {
        this.Lemit = Lemit;
        this.position = position;
        this.normal = normalize(normal);
        this.dpdu = normalize(dpdu);
        this.dpdv = cross(this.normal, this.dpdu);
        this.radius = radius;
        this.area = Pi * radius * radius;
        this.twoSided = twoSided;
    }

    public __init(SampledSpectrum Lemit, float3 position, float3 normal, float radius, bool twoSided = false) {
        this.Lemit = Lemit;
        this.position = position;
        this.normal = normalize(normal);
        buildOrthonormalBasis(this.normal, this.dpdu, this.dpdv);
        this.radius = radius;
        this.area = Pi * radius * radius;
        this.twoSided = twoSided;
    }

    public LightType Type() {
        return LightType::Area;
    }

    public SampledSpectrum L(float3 p, float3 n, float3 uv, float3 w) {
        if (!twoSided && dot(n, w) < 0.0f) return SampledSpectrum(0.0f);
        return Lemit;
    }

    public SampledSpectrum Le(float3 w) {
        return SampledSpectrum(0.0f);
    }

    public void PDF_Le(LightSampleContext ctx, float3 wi, out float pdfPos, out float pdfDir) {
        pdfPos = 0.0f;
        pdfDir = 0.0f;
    }

    public float PDF_Li(LightSampleContext ctx, float3 wi, bool allowIncompletePDF) {
        float cosTheta = dot(normal, wi);
        if (cosTheta == 0.0f) return 0.0f;
        
        // wi is direction from shading point to light.
        // Light normal should face opposite to wi.
        if (!twoSided && cosTheta > 0.0f) return 0.0f;

        // Ray-plane intersection
        float t = dot(normal, position - ctx.p()) / cosTheta;
        if (t <= 0.0f) return 0.0f;

        float3 pDisk = ctx.p() + t * wi;
        float dist2 = dot(pDisk - position, pDisk - position);
        if (dist2 > radius * radius) return 0.0f;

        float pdfArea = 1.0f / area;
        return pdfArea * (t * t) / abs(cosTheta);
    }

    public LightLiSample Sample_Li(LightSampleContext ctx, float2 u) {
        float2 pd = SampleConcentricDisk(u) * radius;
        float3 pDisk = position + pd.x * dpdu + pd.y * dpdv;
        
        float3 wi = pDisk - ctx.p();
        float dist2 = dot(wi, wi);
        if (dist2 == 0.0f) return LightLiSample();
        
        float dist = sqrt(dist2);
        wi /= dist;

        float cosTheta = dot(normal, -wi);
        if (!twoSided && cosTheta <= 0.0f) return LightLiSample();

        // Solid angle PDF
        float pdf = dist2 / (area * abs(cosTheta));
        return LightLiSample(Lemit, wi, pdf, false);
    }
};

public struct RectLight : ILight {
    SampledSpectrum Lemit;
    float3 position;
    float3 normal;
    float3 dpdu;
    float3 dpdv;
    float area;
    bool twoSided;

    public __init(SampledSpectrum Lemit, float3 position, float3 dpdu, float3 dpdv, bool twoSided = false) {
        this.Lemit = Lemit;
        this.position = position;
        this.dpdu = dpdu;
        this.dpdv = dpdv;
        float3 crossProd = cross(dpdu, dpdv);
        this.area = length(crossProd) * 4.0f;
        this.normal = normalize(crossProd);
        this.twoSided = twoSided;
    }

    public LightType Type() {
        return LightType::Area;
    }

    public SampledSpectrum L(float3 p, float3 n, float3 uv, float3 w) {
        if (!twoSided && dot(n, w) < 0.0f) return SampledSpectrum(0.0f);
        return Lemit;
    }

    public SampledSpectrum Le(float3 w) {
        return SampledSpectrum(0.0f);
    }

    public void PDF_Le(LightSampleContext ctx, float3 wi, out float pdfPos, out float pdfDir) {
        pdfPos = 0.0f;
        pdfDir = 0.0f;
    }

    public float PDF_Li(LightSampleContext ctx, float3 wi, bool allowIncompletePDF) {
        float cosTheta = dot(normal, wi);
        if (cosTheta == 0.0f) return 0.0f;
        
        if (!twoSided && cosTheta > 0.0f) return 0.0f;

        // Ray-plane intersection
        float t = dot(normal, position - ctx.p()) / cosTheta;
        if (t <= 0.0f) return 0.0f;

        float3 pRect = ctx.p() + t * wi;
        float3 d = pRect - position;
        
        float l2_u = dot(dpdu, dpdu);
        float l2_v = dot(dpdv, dpdv);
        float u = dot(d, dpdu) / l2_u;
        float v = dot(d, dpdv) / l2_v;

        if (abs(u) > 1.0f || abs(v) > 1.0f) return 0.0f;

        float pdfArea = 1.0f / area;
        return pdfArea * (t * t) / abs(cosTheta);
    }

    public LightLiSample Sample_Li(LightSampleContext ctx, float2 u) {
        float2 pd = u * 2.0f - 1.0f;
        float3 pRect = position + pd.x * dpdu + pd.y * dpdv;
        
        float3 wi = pRect - ctx.p();
        float dist2 = dot(wi, wi);
        if (dist2 == 0.0f) return LightLiSample();
        
        float dist = sqrt(dist2);
        wi /= dist;

        float cosTheta = dot(normal, -wi);
        if (!twoSided && cosTheta <= 0.0f) return LightLiSample();

        // Solid angle PDF
        float pdf = dist2 / (area * abs(cosTheta));
        return LightLiSample(Lemit, wi, pdf, false);
    }
};

// ============================================================
// N-Light Weighted Mixture Sampler (analogous to LobeMixture)
// ============================================================
// Holds up to 4 lights with luminance-based weights.
// Provides weighted Sample_Li / PDF_Li / Le for MIS-correct
// multi-light sampling in a path integrator.
struct LightEntry {
    ILight light;
    float  weight; // unnormalized selection weight
};

public struct LightMixture {
    LightEntry lights[4];
    int        lightCount;
    float      weightSum;

    public __init() {
        lightCount = 0;
        weightSum  = 0.0f;
    }

    // Add a light with an unnormalized importance weight (e.g. luminance).
    // Zero-weight lights are silently skipped.
    [mutating] public void AddLight(ILight l, float w) {
        if (lightCount >= 4 || w <= 0.0f) return;
        lights[lightCount].light  = l;
        lights[lightCount].weight = w;
        lightCount++;
    }

    // Must be called after all AddLight() calls and before sampling.
    [mutating] public void Finalize() {
        weightSum = 0.0f;
        for (int i = 0; i < lightCount; i++)
            weightSum += lights[i].weight;
        if (weightSum <= 0.0f) weightSum = 1.0f;
    }

    // Sample one light by categorical selection on normalized weights.
    // Returns the chosen light's LightLiSample with:
    //   Li   scaled by 1/(chosenPDF * selectionProb)   — ready for MIS
    //   pdf  set to the mixture PDF = Σ_j (w_j/W) * pdf_j
    public LightLiSample Sample_Li(LightSampleContext ctx, float uc, float2 u) {
        if (lightCount == 0 || weightSum <= 0.0f)
            return LightLiSample();

        // 1. Categorical selection
        int chosen = lightCount - 1;
        float cumulative = 0.0f;
        for (int i = 0; i < lightCount; i++) {
            cumulative += lights[i].weight / weightSum;
            if (uc < cumulative) {
                chosen = i;
                break;
            }
        }

        float pChoice = lights[chosen].weight / weightSum;

        // 2. Delegate to chosen light
        LightLiSample Li = lights[chosen].light.Sample_Li(ctx, u);
        if (Li.pdf <= 0.0f)
            return LightLiSample();

        // 3. Scale radiance: Li / (light_pdf * selection_prob)
        SampledSpectrum radiance = Li.Li / (Li.pdf * pChoice);

        bool isDelta = IsDeltaLight(lights[chosen].light.Type());

        // 4. Compute mixture PDF for MIS: Σ_j (w_j/W) * pdf_j(wi)
        float mixPDF = isDelta ? Li.pdf : this.PDF_Li(ctx, Li.wi);

        // 5. Return with mixture PDF
        return LightLiSample(radiance, Li.wi, mixPDF, isDelta);
    }

    // Mixture PDF: Σ_i (w_i / weightSum) * pdf_i(wi)
    public float PDF_Li(LightSampleContext ctx, float3 wi) {
        float result = 0.0f;
        for (int i = 0; i < lightCount; i++) {
            float nw = lights[i].weight / weightSum;
            result += nw * lights[i].light.PDF_Li(ctx, wi, true);
        }
        return result;
    }

    // Environment Le: Σ_i (w_i / weightSum) * Le_i(w)
    // Only Infinite-type lights contribute.
    public SampledSpectrum Le(float3 w) {
        SampledSpectrum result = SampledSpectrum(0.0f);
        for (int i = 0; i < lightCount; i++) {
            if (lights[i].light.Type() == LightType::Infinite)
                result += lights[i].light.Le(w);
        }
        return result;
    }
};