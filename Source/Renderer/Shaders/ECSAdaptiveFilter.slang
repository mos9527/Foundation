import ICommon;

uniform UBO globalParams;

RWTexture2D<float4> adaptiveAux;

[[SpecializationConstant]] uint passIndex; // 0 for X pass, 1 for Y pass

[shader("compute")]
[numthreads(8, 8, 1)]
void ComputeMain(uint3 id : SV_DispatchThreadID)
{
    if (globalParams.adaptiveThreshold <= 0.0f)
        return;

    uint2 pix = id.xy;
    if (pix.x >= uint(globalParams.fbWidth) || pix.y >= uint(globalParams.fbHeight))
        return;

    float4 current = adaptiveAux[pix];
    
    // If we already demand samples, our state is 0.0f.
    // If we are converged (1.0f), we might need to demand samples if our neighbors do.
    if (current.w != 0.0f)
    {
        bool needsSamples = false;
        if (passIndex == 0) // X pass
        {
            if (pix.x > 0 && adaptiveAux[uint2(pix.x - 1, pix.y)].w == 0.0f)
                needsSamples = true;
            if (pix.x < uint(globalParams.fbWidth) - 1 && adaptiveAux[uint2(pix.x + 1, pix.y)].w == 0.0f)
                needsSamples = true;
        }
        else // Y pass
        {
            if (pix.y > 0 && adaptiveAux[uint2(pix.x, pix.y - 1)].w == 0.0f)
                needsSamples = true;
            if (pix.y < uint(globalParams.fbHeight) - 1 && adaptiveAux[uint2(pix.x, pix.y + 1)].w == 0.0f)
                needsSamples = true;
        }

        if (needsSamples)
        {
            // Set to 0.0f to demand samples
            adaptiveAux[pix] = float4(current.xyz, 0.0f);
        }
    }
}
