/**
* Simple CRT shader by kbjwes77
* ---
* https://www.shadertoy.com/view/WsVSzV
*/

Texture2D screen;
SamplerState sampler;

[shader("fragment")]
float4 fragMain(float2 uv: TEXCOORD0) : SV_TARGET {
    float warp = 0.75; // simulate curvature of CRT monitor
    float scan = 0.75; // simulate darkness between scanlines
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
    	{
        // determine if we are drawing in a scanline
        float apply = abs(sin(p.y)*0.5*scan);
        // sample the texture
    	out = float4(lerp(screen.Sample(sampler, uv).rgb,float3(0.0),apply),1.0);
        }

    return out;
}
