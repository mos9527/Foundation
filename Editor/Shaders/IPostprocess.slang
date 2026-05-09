import ITonemapping;

public struct PostprocessInfo
{
    public float Exposure;
    public uint  HdrEnabled;
    public float HdrPaperWhiteNits;
}

// BT.709/sRGB RGB -> XYZ Y row.
public static const float3 BT709_S = float3(0.212639006f, 0.715168679f, 0.072192315f);

// Luminance-preserving EV exposure. Keeps chroma stable across changes in EV
// without per-channel hue shifts.
public float3 ApplyExposureEV(float3 L, float ev) {
    float Y = max(dot(L, BT709_S), 1e-6f);
    float Yp = Y * exp2(ev);
    return L * (Yp / Y);
}

// L: BT709 Linear primaries content
public float3 Postprocess(float3 L /* Linear */, PostprocessInfo pp,
                          Texture3D<float4> viewLutSdr, Texture3D<float4> viewLutHdr,
                          SamplerState lutSampler) {
    L = ApplyExposureEV(L, pp.Exposure);
    TonemapInfo info;
    info.HdrPaperWhiteNits = pp.HdrPaperWhiteNits;
    AcesLutTonemapper op;
    if (pp.HdrEnabled != 0)
        return op.Perform(info, L, viewLutHdr, lutSampler);
    return op.Perform(info, L, viewLutSdr, lutSampler);
}
