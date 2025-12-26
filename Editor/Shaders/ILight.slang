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
    public __init(SampledSpectrum Li = 0, float3 wi = 0, float pdf = 0) {
        this.Li = Li;
        this.wi = wi;
        this.pdf = pdf;
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
};
// -- Lights
// https://pbr-book.org/4ed/Light_Sources/Infinite_Area_Lights#UniformInfiniteLights
public struct UniformInfiniteLight : ILight {
    SampledSpectrum Lemit;
    float scale;
    public __init(SampledSpectrum Lemit, float scale){
        this.Lemit = Lemit;
        this.scale = scale;
    }
    public LightType Type() {
        return LightType::Infinite;
    }
    // Not defined
    public SampledSpectrum L(float3 p, float3 n, float3 uv, float3 w) {
        return {};
    }
    public SampledSpectrum Le(float3 w) {
        return Lemit * scale;
    }
    public void PDF_Le(LightSampleContext, float3, out float pdfPos, out float pdfDir){
        pdfPos = 0.0f;
        pdfDir = UniformSpherePDF();
    }
    public LightLiSample Sample_Li(LightSampleContext ctx, float2 u) {
        float3 wi = SampleUniformSphere(u);
        return LightLiSample(Lemit, wi, UniformSpherePDF());
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
        return LightLiSample(Lemit, -direction, 1.0f);
    }
};