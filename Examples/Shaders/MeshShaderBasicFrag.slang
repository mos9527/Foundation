struct V2F
{
    float4 pos : SV_POSITION;
    uint32_t meshlet : ID;
};
//
// Fragment Shader
//
uint hash(uint a)
{
    a = (a + 0x7ed55d16) + (a << 12);
    a = (a ^ 0xc761c23c) ^ (a >> 19);
    a = (a + 0x165667b1) + (a << 5);
    a = (a + 0xd3a2646c) ^ (a << 9);
    a = (a + 0xfd7046c5) + (a << 3);
    a = (a ^ 0xb55a4f09) ^ (a >> 16);
    return a;
}
float3 hash3(uint a){
    a = hash(a);
    return float3(float(a & 255), float((a >> 8) & 255), float((a >> 16) & 255)) / 255.0;
}

struct Fragment
{
    float4 color : SV_Target;
};

[shader("fragment")]
Fragment fragMain(V2F input)
{
    Fragment output;
    output.color = float4(hash3(input.meshlet), 1.0);
    return output;
}
