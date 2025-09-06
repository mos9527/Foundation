/**
 * Mandelbrot - smooth by iq
 * ---
 * https://iquilezles.org/articles/msetsmooth/
 * https://www.shadertoy.com/view/4df3Rn
*/
RWTexture2D<float4> image;

struct PC {
    float time;
    int2 resolution;
};
[[vk::push_constant]] PC pc;

// compute the smooth iteration count for a pointin the plane
float mandelbrot(in float2 c)
{
    const float B = 256.0;

    float n = 0.0;
    float2 z = float2(0.0);
    for (int i = 0; i < 200; i++)
    {
        z = float2(z.x * z.x - z.y * z.y, 2.0 * z.x * z.y) + c; // z = z2 + c
        if (dot(z, z) > (B * B)) break;
        n += 1.0;
    }

    return n - log(log(length(z)) / log(B)) / log(2.0); // smooth iteration count
    return n - log2(log2(dot(z, z))) + 4.0;             // equivalent optimized smooth iteration count
}

[shader("compute")]
[numthreads(32, 32, 1)]
void csMain(uint2 dispatchThreadId : SV_DispatchThreadID)
{
    int2 coord = int2(dispatchThreadId);
    float2 p = (-pc.resolution + 2.0 * coord) / pc.resolution.yy;

    float time = pc.time;

    float zoo = 0.62 + 0.38 * cos(.07 * time);
    float coa = cos(0.15 * (1.0 - zoo) * time);
    float sia = sin(0.15 * (1.0 - zoo) * time);
    zoo = pow(zoo, 8.0);
    float2 xy = float2(p.x * coa - p.y * sia, p.x * sia + p.y * coa);
    float2 c = float2(-.745, .186) + xy * zoo;

    float l = mandelbrot(c);

    float3 col = (l < 0.5) ? float3(0.0, 0.0, 0.0) : 
               0.5 + 0.5 * cos(3.0 + l * 0.15 + float3(0.0, 0.6, 1.0));

    image[coord] = float4(col, 1.0);
}
