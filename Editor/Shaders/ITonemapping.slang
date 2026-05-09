// NOTE: Assume Linear BT.709/sRGB for linear color primaries

// Linear BT.709/sRGB to linear BT.2020, for HDR10/PQ output.
// See also Scripts/ColorPrimaries.ipynb
public static const float3x3 BT709_BT2020 = float3x3(
    0.627403896f, 0.329283038f, 0.043313066f,
    0.069097289f, 0.919540395f, 0.011362316f,
    0.016391439f, 0.088013308f, 0.895595253f);

// Linear BT.709/D65 -> AP1/D60 with Bradford chromatic adaptation.
// Derived in Scripts/ColorPrimaries.ipynb; matches Scripts/OCIOBakeLUTs.py.
public static const float3x3 BT709_AP1_BFD = float3x3(
    0.613097429f, 0.339523137f, 0.047379453f,
    0.070193723f, 0.916353881f, 0.013452398f,
    0.020615593f, 0.109569773f, 0.869814634f);

// ACEScct log encoding with linear toe near black.
// https://docs.acescentral.com/encodings/acescct/#encoding-function
public float3 ACEScctEncode(float3 x) {
    float3 toe = 10.5402377416545f * x + 0.0729055341958355f;
    float3 logp = (log2(max(x, 0x1p-16f)) + 9.72f) / 17.52f;
    float3 sel = float3(x > float3(0.0078125f));
    return lerp(toe, logp, sel);
}

// Map exposed linear BT.709 RGB into the OCIO-baked 3D LUT input domain.
// LUT input is raw ACEScct in [0,1]. See Scripts/OCIOBakeLUTs.py.
public float3 LinearBT709ToAcescctCoord(float3 rgbBT709) {
    float3 ap1 = mul(BT709_AP1_BFD, rgbBT709);
    return saturate(ACEScctEncode(ap1));
}

public struct TonemapInfo
{
    public float HdrPaperWhiteNits;
}


public interface IOOTF {
    float3 Transfer(TonemapInfo info, float3 L);
}

public interface IEOTF {
    float3 Inverse(TonemapInfo info, float3 L);
}

public interface ITonemapper {
    float3 Perform(TonemapInfo info, float3 L, Texture3D<float4> viewLut, SamplerState lutSampler);
}

// BT709->BT709
// Identity OOTF
public struct SrgbOOTF : IOOTF {
    float3 Transfer(TonemapInfo info, float3 x) {
        return x;
    }
}
// https://www.w3.org/Graphics/Color/srgb
// https://registry.khronos.org/DataFormat/specs/1.3/dataformat.1.3.html#TRANSFER_CONVERSION
public struct SrgbEOTF : IEOTF {
    float3 Inverse(TonemapInfo info, float3 L) {
        L = saturate(L);
        float3 Ep = 1.055f * pow(L, 1.0f / 2.4f) - 0.055f;
        float3 sel = float3(L > float3(0.0031308f));
        Ep = lerp(12.92f * L, Ep, sel);
        return Ep;
    }
}

// Reference PQ OOTF
// https://www.itu.int/dms_pubrec/itu-r/rec/bt/R-REC-BT.2100-3-202502-I!!PDF-C.pdf
// Post BT709->BT2020 OOTF
public struct PqOOTF : IOOTF {
    float3 Transfer(TonemapInfo info, float3 x) {
        x = mul(BT709_BT2020, x);
        // E' = G709[E], E=x, post exposure, in [0,1]
        // Note that when E=1.0
        // E' = 6.81, Fd = 100 * (6.81)^2.4 = 9989.714 Nits
        // This is _NOT_ our definition of E, where x=1.0 maps to  paperwhite brightness instead
        // Solve for Fd = PaperWhite -> E' = 1 -> pow(59.5208x', 0.45) = 1.0
        // So div first to linearly map x' to x
        x = x / 59.5208f;
        x = saturate(x);
        float3 Ep = 1.099 * pow((59.5208f * x), 0.45f) - 0.099;
        float3 sel = float3(x > float3(0.0003024));
        Ep = lerp(267.84 * x, Ep, sel);
        float3 Fd = info.HdrPaperWhiteNits * pow(Ep, 2.4f); // NOTE: default paper white=100nits by the doc
        return Fd;
    }
}

// SMPTE ST 2084 (PQ) Inverse EOTF
// https://www.itu.int/dms_pubrec/itu-r/rec/bt/R-REC-BT.2100-3-202502-I!!PDF-C.pdf
public struct PqEOTF : IEOTF {
    float3 Inverse(TonemapInfo info, float3 L) {
        const float m1 = 2610.0f / 16384.0f;
        const float m2 = (2523.0f / 4096.0f) * 128.0f;
        const float c1 = 3424.0f / 4096.0f;
        const float c2 = (2413.0f / 4096.0f) * 32.0f;
        const float c3 = (2392.0f / 4096.0f) * 32.0f;

        // Normalize absolute nits to [0, 1] where 1.0 = 10000 nits
        float3 Y = max(L.xyz / 10000.0f, 0.0f);
        float3 Y_m1 = pow(Y, m1);
        float3 N = (c1 + c2 * Y_m1) / (1.0f + c3 * Y_m1);
        float3 pq = pow(N, m2);
        
        return pq;
    }
}

public struct Tonemapper<OO, EO> : ITonemapper
    where OO : IOOTF
    where EO : IEOTF
{
    float3 Perform(TonemapInfo info, float3 L /* linear */, Texture3D<float4> viewLut, SamplerState lutSampler){
        OO o = OO();
        EO e = EO();
        L = o.Transfer(info, L);
        L = e.Inverse(info, L);
        return L;
    }
}

public typealias StandardSrgbTonemapper = Tonemapper<SrgbOOTF, SrgbEOTF> ;
public typealias StandardPqTonemapper = Tonemapper<PqOOTF, PqEOTF> ;

// Display transform via OCIO-baked 3D LUT.
// Replaces the entire OOTF + inverse EOTF stage; the LUT outputs encoded display RGB.
// See Scripts/OCIOBakeLUTs.py for the bake. The runtime shaper here must match
// Scripts/ColorPrimaries.ipynb's BT.709/D65 -> AP1/D60 Bradford derivation.
public struct ViewLutTonemapper : ITonemapper
{
    float3 Perform(TonemapInfo info, float3 L /* scene-linear BT.709, post-exposure */,
                   Texture3D<float4> viewLut, SamplerState lutSampler) {
        float3 coord = LinearBT709ToAcescctCoord(L);
        return viewLut.SampleLevel(lutSampler, coord, 0).rgb;
    }
}
