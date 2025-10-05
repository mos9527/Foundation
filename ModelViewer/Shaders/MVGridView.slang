import "Common/Definitions";
struct Params {
    CameraParams camera;
    uint dimension;
    float width;
    uint type; // 0 = cartesian, 1 = radial
}
[[vk::push_constant]] Params params;

static float2 positions[6] = float2[](
    // CCW
    float2(0, 1),
    float2(-1, 1),
    float2(-1, 0),

    float2(0,0),
    float2(0,1),
    float2(-1,0),
);
static float2 offsets[4] = float2[](
    float2(0,0),
    float2(1,0),
    float2(0,-1),
    float2(1,-1)
);
struct GridVertex {
    float4 pos : SV_Position;
    float2 world;
}
[shader("vertex")]
GridVertex vertMain(uint vid: SV_VertexID /* 24 */ ) {
    GridVertex output;    
    float2 vtx = positions[vid % 6] + offsets[vid / 6];    
    vtx = vtx * params.dimension + floor(params.camera.cameraPosition.xy); // Grid shader works on units of 1 world space
    output.pos = mul(params.camera.ViewProj(), float4(vtx, 0, 1.0));    
    output.world = vtx;
    return output;
}
// From Ben Golus's https://bgolus.medium.com/the-best-darn-grid-shader-yet-727f9278b9d8
float PristineGrid(float2 world, float2 targetWidth)
{
    float2 uv = 1 - abs(frac(world) * 2.0 - 1.0);
    // Per-pixel delta in world space
    float2 dsize = fwidth(world);
    float2 drawWidth = clamp(targetWidth, dsize, 0.5);
    float2 lineAA = max(dsize, 1e-5) * 1.5;    
    float2 grid2 = smoothstep(drawWidth + lineAA, drawWidth - lineAA, uv);
    // Fade out grid as it gets further away
    grid2 *= saturate(targetWidth / drawWidth);
    // Suppress Morie 
    grid2 = lerp(grid2, targetWidth, saturate(dsize * 2.0 - 1.0));
    return lerp(grid2.x, 1.0, grid2.y);
}
float Grid(float2 world, float width)
{
    float2 uv = world;
    return PristineGrid(uv, float2(width));
}
float RadialGrid(float2 world, float width)
{
    float angle = abs(atan2(world.y, world.x)) / acos(-1);
    float r = length(world);
    float2 uv = float2(angle * 16, r);
    return PristineGrid(uv, float2(width * 16 / r / acos(-1) , width));
}
[shader("fragment")]
float4 fragMain(GridVertex vtx) : SV_Target
{
    float grid = 0;
    if (params.type == 0)
        grid = Grid(vtx.world, params.width);
    else
        grid = RadialGrid(vtx.world, params.width);
    return float4(float3(grid), 0.5);
}
