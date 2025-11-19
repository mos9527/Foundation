struct V2F
{
    float4 pos : SV_POSITION;
    float4 color : COLOR0;
};
//
// Fragment Shader
//

struct Fragment
{
    float4 color : SV_Target;
};

[shader("fragment")]
Fragment fragMain(V2F input)
{
    Fragment output;
    output.color = float4(input.color.rgb, 1.0);
    return output;
}
