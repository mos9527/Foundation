 /**
 * Ported from iq's SDF 2D example
 * ---
 * https://iquilezles.org/articles/distfunctions2d/
*/

[[vk::push_constant]] float time;

float dot2(in float2 v) { return dot(v, v); }
float sdTriangle(in float2 p, in float2 p0, in float2 p1, in float2 p2)
{
    float2 e0 = p1 - p0, v0 = p - p0; float d0 = dot2(v0 - e0 * clamp(dot(v0, e0) / dot(e0, e0), 0.0, 1.0));
    float2 e1 = p2 - p1, v1 = p - p1; float d1 = dot2(v1 - e1 * clamp(dot(v1, e1) / dot(e1, e1), 0.0, 1.0));
    float2 e2 = p0 - p2, v2 = p - p2; float d2 = dot2(v2 - e2 * clamp(dot(v2, e2) / dot(e2, e2), 0.0, 1.0));

    float o = e0.x * e2.y - e0.y * e2.x;
    float2 d = min(min(float2(d0, o * (v0.x * e0.y - v0.y * e0.x)),
                     float2(d1, o * (v1.x * e1.y - v1.y * e1.x))),
                 float2(d2, o * (v2.x * e2.y - v2.y * e2.x)));
    return -sqrt(d.x) * sign(d.y);
}

// 2D SDF drawing
[shader("fragment")]
float4 fragMain(float2 p: TEXCOORD0) : SV_Target
{
    p = (p - float2(0.5)) * 3.0;

    // animate
    float2 v1 = float2(1.4, 1.0) * cos(time + float2(0.0, 2.00) + 0.0);
    float2 v2 = float2(1.4, 1.0) * cos(time + float2(0.0, 1.50) + 1.5);
    float2 v3 = float2(1.4, 1.0) * cos(time + float2(0.0, 3.00) + 4.0);

    // distance
    float d = sdTriangle(p, v1, v2, v3);

    // color
    float3 col = float3(1.0) - sign(d) * float3(0.1, 0.4, 0.7);
    col *= 1.0 - exp(-2.0 * abs(d));
    col *= 0.8 + 0.2 * cos(120.0 * d);
    col = lerp(col, float3(1.0), 1.0-smoothstep(0.0,0.02,abs(d)) );

    return float4(col, 1.0);
}
