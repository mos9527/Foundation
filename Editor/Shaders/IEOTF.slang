interface IOOTF {
    float3 Transfer(float3 L);
}

interface IEOTF {
    float3 Inverse(float3 L);
}

interface ITonemapper {
    float3 Perform(float3 L);
}

struct StandardOOTF : IOOTF {
    float3 Transfer(float3 x) {
        return x;
    }
}
// https://knarkowicz.wordpress.com/2016/01/06/aces-filmic-tone-mapping-curve/
struct AcesOOTF : IOOTF {
    float3 Transfer(float3 x) {
        float a = 2.51f;
        float b = 0.03f;
        float c = 2.43f;
        float d = 0.59f;
        float e = 0.14f;
        return saturate((x.xyz*(a*x.xyz+b))/(x.xyz*(c*x.xyz+d)+e));
    }
}

// https://registry.khronos.org/DataFormat/specs/1.3/dataformat.1.3.html#TRANSFER_CONVERSION
struct SrgbEOTF : IEOTF {
    float3 Inverse(float3 L) {
        // Simple gamma 2.2 inverse EOTF
        return pow(L.xyz, 1.0f/2.2f);
    }
}

// SMPTE ST 2084 (PQ) Inverse EOTF
// https://www.itu.int/dms_pubrec/itu-r/rec/bt/R-REC-BT.2100-3-202502-I!!PDF-C.pdf
struct PqEOTF : IEOTF {
    float3 Inverse(float3 L) {
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

struct Tonemapper<OO, EO> : ITonemapper
    where OO : IOOTF
    where EO : IEOTF
{
    float3 Perform(float3 L /* linear */){
        OO o = OO();
        EO e = EO();
        L = o.Transfer(L);
        L = e.Inverse(L);
        return L;
    }
}

typedef Tonemapper<AcesOOTF, SrgbEOTF> AcesSrgbTonemapper;
typedef Tonemapper<StandardOOTF, PqEOTF> StandardPqTonemapper;
