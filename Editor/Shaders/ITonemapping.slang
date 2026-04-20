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
    float3 Perform(TonemapInfo info, float3 L);
}

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
public struct PqOOTF : IOOTF {
    float3 Transfer(TonemapInfo info, float3 x) {
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
    float3 Perform(TonemapInfo info, float3 L /* linear */){
        OO o = OO();
        EO e = EO();
        L = o.Transfer(info, L);
        L = e.Inverse(info, L);
        return L;
    }
}

public typealias StandardSrgbTonemapper = Tonemapper<SrgbOOTF, SrgbEOTF> ;
public typealias StandardPqTonemapper = Tonemapper<PqOOTF, PqEOTF> ;
