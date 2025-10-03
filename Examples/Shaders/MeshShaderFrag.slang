struct Vertex
{
  float4 pos : SV_Position;
  float3 color : Color;
  int index : Index;
  int value : Value;
};

//
// Fragment Shader
//

struct Fragment
{
    float4 color : SV_Target;
};

[shader("fragment")]
Fragment fragMain(Vertex input)
{
    Fragment output;
    output.color = float4(input.color, 1.0);
    return output;
}
