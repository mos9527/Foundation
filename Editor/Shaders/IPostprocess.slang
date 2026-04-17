import IEOTF;

public struct PostprocessInfo
{
    public float Exposure;
    public uint  HdrEnabled;
    public float HdrPaperWhiteNits;
}

public float3 Tonemap(float3 L,PostprocessInfo pp) {
    L *= exp2(pp.Exposure);
    ITonemapper op;
    if (pp.HdrEnabled) {
        L.xyz = L.xyz * pp.HdrPaperWhiteNits;
        op = StandardPqTonemapper();
    } else {
        op = AcesSrgbTonemapper();
    }
    return op.Perform(L);
}

public float3 Postprocess(float3 L /* Linear */, PostprocessInfo pp) {
    L = Tonemap(L, pp);
    return L;
}
