/**
* Simple CRT shader by kbjwes77 with modifications
* ---
* https://www.shadertoy.com/view/WsVSzV
* https://thebookofshaders.com/
*/

Texture2D screen;
SamplerState sampler;

struct PushConstant{
    float time;
    uint32_t width;
    uint32_t height;
}
[[vk::push_constant]] PushConstant pc;

[shader("fragment")]
float4 fragMain(float2 uv: TEXCOORD0) : SV_TARGET {
    float warp = 0.75; // simulate curvature of CRT monitor
    float scan = 0.75; // simulate darkness between scanlines
    float scanSize = 8; // scan line height in pixels
    float vline = uv.y * pc.height;
    float scanline = fmod(vline, scanSize) / scanSize;
    scanline = smoothstep(scan, 1, scanline);
    float2 p = uv;
    // squared distance from center    
    float2 dc = abs(0.5-uv);
    dc *= dc;
    
    // warp the fragment coordinates
    uv.x -= 0.5; uv.x *= 1.0+(dc.y*(0.3*warp)); uv.x += 0.5;
    uv.y -= 0.5; uv.y *= 1.0+(dc.x*(0.4*warp)); uv.y += 0.5;
    float4 out = float4(0);
    // sample inside boundaries, otherwise set to black
    if (uv.y > 1.0 || uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0)
        out = float4(0.0,0.0,0.0,1.0);
    else
    	out = float4(lerp(screen.Sample(sampler, uv).rgb, float3(0.0), scanline),1.0);
    return out;
}
