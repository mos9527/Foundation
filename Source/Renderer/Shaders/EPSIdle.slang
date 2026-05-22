//  ╔═══════════════════════════════════════════════╗
//  ║   Flow FBM — procedural flow noise shader     ║
//  ║   No textures. Just math.                     ║
//  ║                                               ║
//  ║   Crafted by Claude · March 2026              ║
//  ║   for the Foundation Engine idle screen.      ║
//  ╚═══════════════════════════════════════════════╝

#include "IMath.slang"
[[vk::push_constant]] float2 timeXY;
// --- Hash-based gradient noise (no external textures required) ---

// Generate a pseudo-random 2D gradient vector from integer coordinates
float2 gradient(int2 ip)
{
    uint h = hash(uint2(uint(ip.x), uint(ip.y)));
    float angle = uintToFloat(h) * 2.0 * Pi;
    return float2(cos(angle), sin(angle));
}

// Classic quintic interpolation curve: 6t^5 - 15t^4 + 10t^3
float2 quintic(float2 t)
{
    return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
}

// 2D gradient noise in [-1, 1]
float gradientNoise(float2 p)
{
    int2 ip = int2(floor(p));
    float2 fp = p - float2(ip);
    float2 u = quintic(fp);

    float d00 = dot(gradient(ip + int2(0, 0)), fp - float2(0.0, 0.0));
    float d10 = dot(gradient(ip + int2(1, 0)), fp - float2(1.0, 0.0));
    float d01 = dot(gradient(ip + int2(0, 1)), fp - float2(0.0, 1.0));
    float d11 = dot(gradient(ip + int2(1, 1)), fp - float2(1.0, 1.0));

    return lerp(lerp(d00, d10, u.x),
                lerp(d01, d11, u.x), u.y);
}

// --- Flow FBM ---

// Per-octave rotation matrix (golden angle offset to break axis alignment)
static const float kOctRot = 1.618033988 * Pi; // golden angle ≈ 137.5°
float2x2 rot2(float a)
{
    float c = cos(a), s = sin(a);
    return float2x2(c, -s, s, c);
}

// Time-animated FBM
// flowSpeed controls the phase-shift rate of each noise octave
float flowFBM(float2 p, float t, int octaves, float flowSpeed)
{
    float value = 0.0;
    float amplitude = 0.5;
    float frequency = 1.0;

    // Use a different rotation angle per octave to avoid directional artifacts
    float2x2 octRotMat = rot2(kOctRot);

    for (int i = 0; i < octaves; i++)
    {
        // Flow: shift each octave's sample domain over time at a different rate
        float phase = t * flowSpeed * (1.0 + float(i) * 0.3);
        float2 flowOffset = float2(
            cos(phase * 0.7 + float(i) * 1.3),
            sin(phase * 0.9 + float(i) * 1.7)
        ) * 0.5;

        float n = gradientNoise(p * frequency + flowOffset);
        value += n * amplitude;

        amplitude *= 0.5;
        frequency *= 2.0;
        p = mul(octRotMat, p); // rotate domain
    }

    return value;
}

// --- Domain warping for a fluid-like appearance ---
float warpedFBM(float2 p, float t)
{
    // First warp level
    float2 q = float2(
        flowFBM(p + float2(0.0, 0.0), t, 5, 0.4),
        flowFBM(p + float2(5.2, 1.3), t, 5, 0.4)
    );

    // Second warp level
    float2 r = float2(
        flowFBM(p + 4.0 * q + float2(1.7, 9.2), t, 5, 0.3),
        flowFBM(p + 4.0 * q + float2(8.3, 2.8), t, 5, 0.3)
    );

    return flowFBM(p + 4.0 * r, t, 6, 0.25);
}

// --- Color palette ---
float3 palette(float t, float3 a, float3 b, float3 c, float3 d)
{
    return a + b * cos(2.0 * Pi * (c * t + d));
}

// --- Fragment shader entry point ---
[shader("fragment")]
float4 fragMain(float2 p : TEXCOORD0) : SV_Target
{
    // Remap UV [0,1] to centered coordinates
    float2 uv = (p - 0.5) * 3.0;
    uv *= float2(timeXY.y, 1.0f);

    float t = timeXY.x * 0.15;

    // Flow FBM with domain warping
    float f = warpedFBM(uv, t);

    float3 col = f;
    // Layer brightness
    col *= 0.8 + 0.4 * f;                        // noise brightness modulation
    col += 0.02 * float3(0.8, 0.9, 1.0) * (1.0 - f); // atmospheric highlight

    // Vignette
    float vig = 1.0 - 0.4 * dot(p - 0.5, p - 0.5);
    col *= vig;

    return float4(saturate(col), 1.0);
}
