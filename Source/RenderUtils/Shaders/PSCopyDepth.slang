Texture2D<float> srcDepth;

[shader("fragment")]
float fragMain(float4 pos : SV_Position) : SV_Depth
{
    return srcDepth.Load(int3(int2(pos.xy), 0));
}
