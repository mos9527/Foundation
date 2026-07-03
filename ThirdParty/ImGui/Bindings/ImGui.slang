import ITonemapping;

struct Vertex
{
    float4 pos : SV_Position;
    float4 color;
    float2 uv;
    uint textureId;
    uint samplerId;
};
// -- SDF / procedural UI presets --
// The quad's UV is expected to span [0,1] over the rect; rect size in pixels is carried in the params
// so all SDF math happens in pixel units (=> resolution-independent, analytically antialiased).
struct SdfParams
{
    float4 rectRound; // xy: quad size in px, z: corner rounding in px,
                      // w: preset kind | (cornerMask << 8), as float. cornerMask bits: 1=TL 2=TR 4=BL 8=BR
    float4 colA;      // primary color (top of gradient), gamma-encoded like all ImGui colors
    float4 colB;      // secondary color (bottom of gradient / shadow color)
    float4 params;    // xy: preset-specific (see kSdfPreset*), w: quad padding in px
                      //     (how far the quad extends past the logical rect on each side; 0 for most presets)
};

struct PushConstants
{
    float4 scaleTranslate; // xy: scale, zw: translation
    uint4 textureSampler;  // x: texture id, y: sampler id
    SdfParams sdfParams;
};

[[vk::push_constant]] PushConstants pc;

SamplerState linSampler;
SamplerState nearSampler;

[[vk::binding(0, 1)]] // [binding, set]
texture2D textures[];

// Keep in sync with ImGuiSdfPreset in imgui.h
static const int kSdfPresetBox = 0;         // AA rounded fill, colA->colB vertical gradient
static const int kSdfPresetDropShadow = 1;  // soft exterior shadow. params.x = softness px
static const int kSdfPresetInnerShadow = 2; // soft interior shadow at edges. params.x = softness px
static const int kSdfPresetBevel = 3;       // embossed fill w/ lighting. params.x = bevel width px, params.y = strength [0,1]
static const int kSdfPresetGloss = 4;       // glassy fill w/ top highlight band
static const int kSdfPresetNoise = 5;       // grainy fill. params.x = intensity [0,1], params.y = grain size px

float SdRoundBox(float2 p, float2 b, float r)
{
    float2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

// Rounded box with per-corner rounding control (mask bits: 1=TL 2=TR 4=BL 8=BR)
float SdRoundBoxMasked(float2 p, float2 b, float r, uint mask)
{
    uint corner = (p.x > 0.0)
        ? ((p.y > 0.0) ? 8u : 2u)  // BR : TR (y grows downward in UI space)
        : ((p.y > 0.0) ? 4u : 1u); // BL : TL
    float cr = ((mask & corner) != 0u) ? r : 0.0;
    float2 q = abs(p) - b + cr;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - cr;
}

float Hash21(float2 p)
{
    // Cheap 2D hash, good enough for UI grain
    float3 p3 = fract(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float4 EvalSdfPreset(SdfParams prm, float2 uv)
{
    float2 size = prm.rectRound.xy;
    uint kindMask = uint(prm.rectRound.w);
    int kind = int(kindMask & 0xFFu);
    uint cornerMask = (kindMask >> 8) & 0xFu;
    float pad = prm.params.w;
    float2 p = (uv - 0.5) * size;   // pixel units, origin at quad center
    float2 b = size * 0.5 - pad;    // logical rect half-extents (quad may be inflated, e.g. drop shadows)
    float rounding = min(prm.rectRound.z, min(b.x, b.y));
    float d = SdRoundBoxMasked(p, b, rounding, cornerMask);
    // ~1px analytic antialiasing (p is in pixels)
    float aa = 1.0 - smoothstep(-0.75, 0.75, d);
    switch (kind)
    {
    case kSdfPresetDropShadow:
    {
        float soft = max(prm.params.x, 0.001);
        float a = 1.0 - smoothstep(-soft, soft, d);
        a *= a; // quadratic falloff reads softer
        return float4(prm.colA.rgb, prm.colA.a * a);
    }
    case kSdfPresetInnerShadow:
    {
        float soft = max(prm.params.x, 0.001);
        float t = smoothstep(-soft, 0.0, d); // 0 deep inside -> 1 at edge
        return float4(prm.colA.rgb, prm.colA.a * t * t * aa);
    }
    case kSdfPresetBevel:
    {
        float bw = max(prm.params.x, 0.5);
        float strength = saturate(prm.params.y);
        // Numeric SDF gradient -> surface normal. Slope ramps within the bevel band only.
        float e = 1.0;
        float dx = SdRoundBoxMasked(p + float2(e, 0), b, rounding, cornerMask) - SdRoundBoxMasked(p - float2(e, 0), b, rounding, cornerMask);
        float dy = SdRoundBoxMasked(p + float2(0, e), b, rounding, cornerMask) - SdRoundBoxMasked(p - float2(0, e), b, rounding, cornerMask);
        float slope = 1.0 - saturate(-d / bw); // 1 at edge -> 0 interior
        slope *= slope;
        float3 n = normalize(float3(float2(dx, dy) * 0.5 * slope, 1.0 - 0.75 * slope));
        float3 l = normalize(float3(-0.45, -0.6, 0.65)); // top-left key light
        float diff = saturate(dot(n, l));
        float3 h = normalize(l + float3(0, 0, 1));
        float spec = pow(saturate(dot(n, h)), 24.0) * strength * slope;
        float4 col = lerp(prm.colA, prm.colB, uv.y);
        float shade = lerp(1.0, 0.35 + 0.9 * diff, slope * strength);
        return float4(col.rgb * shade + spec, col.a * aa);
    }
    case kSdfPresetGloss:
    {
        float4 col = lerp(prm.colA, prm.colB, uv.y);
        // Top glass highlight band with a hard-ish edge at ~45% height
        float hl = (1.0 - smoothstep(0.35, 0.47, uv.y)) * lerp(0.30, 0.10, saturate(uv.y * 2.2));
        // Faint bottom bounce light
        float bounce = smoothstep(0.85, 1.0, uv.y) * 0.06;
        return float4(col.rgb + hl + bounce, col.a * aa);
    }
    case kSdfPresetNoise:
    {
        float grain = max(prm.params.y, 1.0);
        float g = Hash21(floor(uv * size / grain));
        float4 col = lerp(prm.colA, prm.colB, uv.y);
        col.rgb *= 1.0 + (g - 0.5) * 2.0 * saturate(prm.params.x);
        return float4(col.rgb, col.a * aa);
    }
    case kSdfPresetBox:
    default:
    {
        float4 col = lerp(prm.colA, prm.colB, uv.y);
        return float4(col.rgb, col.a * aa);
    }
    }
}
// ^^^ SDF / procedural UI presets

[shader("vertex")]
Vertex vertMain(
    float2 pos   : POSITION,
    float2 uv    : TEXCOORD,
    float4 color : COLOR)
{
    Vertex output;
    output.pos = float4(pos * pc.scaleTranslate.xy + pc.scaleTranslate.zw, 0.0, 1.0);
    output.color = color;
    output.uv = uv;
    output.textureId = pc.textureSampler.x;
    output.samplerId = pc.textureSampler.y;
    return output;
}

// Tonemap ImGui outputs to these colorspaces
// On Blending (execessively detailed since it's something we've been dealing with at work):
//  - Our output is ALWAYAS in whatever encoding the *colorspace* set by flags is
//  - Our framebuffer format (backbuffer in this case) is ALWAYAS UNORM
//  - Blending HW doesn't care about colorspace and it's always done linearlly... (unless it's sRGB formats)
// For opaque UI this is correct. But for alpha-blended ones we actually produce *different* results for sRGB/HDR displays:
//  > SDR/sRGB is Gamma blending (as in Photoshop/Figma) where colors are always encoded in Gamma space and linearly blended.
//  > HDR/PQ is done in PQ encoded colors and again linearly blended. Non-standard and to put it plainly is simply wrong.
// Gamma blending _is_ ideal for UI elements.
//  > HDR can't do that yet without doing things in sRGB and output in PQ (*shudders*)
//  > Should we opt for linear we can do UIs offscreen. (ex. RE Engine https://www.docswell.com/s/CAPCOM_RandD/KENVMJ-RE2023#p34)
// For ImGui elements we'll let the wrong blending happen. Artists don't use our tool for UI anyways other than getting ground-truth renderings w/o UI.
static const int kFlagsOutputSrgb = 1 << 0;
static const int kFlagsOutputSt2084 = 1 << 1;
[[SpecializationConstant]] int flags;

[shader("fragment")]
float4 fragMain(Vertex vtx) : SV_Target
{
    float4 sample;
    switch (vtx.samplerId){
        case 0:
            sample = textures[vtx.textureId].Sample(linSampler, vtx.uv);
            break;
        case 1:
            sample = textures[vtx.textureId].Sample(nearSampler, vtx.uv);
            break;
        case 2:
            sample = EvalSdfPreset(pc.sdfParams, vtx.uv);
            break;
    }
    sample = vtx.color * sample;
    TonemapInfo info;
    if ((flags & kFlagsOutputSt2084) != 0) {
        PqOOTF ootf;
        PqEOTF eotf;
        // 1.0 -> Paperwhite
        // We take linear input. So invert (gamma-linear) first.
        SrgbEOTF srgb;
        sample.rgb = srgb.Forward(info, sample.rgb);
        float3 L = ootf.Transfer(info, sample.rgb);
        return float4(eotf.Inverse(info, L), sample.a);
    } else {
        // Output is already gamma-encoded.
        return sample;
    }
}
