// Source port of Falcor's BC7 sample
// See https://github.com/NVIDIAGameWorks/Falcor/blob/master/scripts/python/TinyBC/TinyBC.cs.slang
// All copyright notices and license terms are retained from the original source.
/***************************************************************************
 # Copyright (c) 2015-23, NVIDIA CORPORATION. All rights reserved.
 #
 # Redistribution and use in source and binary forms, with or without
 # modification, are permitted provided that the following conditions
 # are met:
 #  * Redistributions of source code must retain the above copyright
 #    notice, this list of conditions and the following disclaimer.
 #  * Redistributions in binary form must reproduce the above copyright
 #    notice, this list of conditions and the following disclaimer in the
 #    documentation and/or other materials provided with the distribution.
 #  * Neither the name of NVIDIA CORPORATION nor the names of its
 #    contributors may be used to endorse or promote products derived
 #    from this software without specific prior written permission.
 #
 # THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS "AS IS" AND ANY
 # EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 # IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 # PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR
 # CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 # EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 # PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 # PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 # OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 # (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 # OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 **************************************************************************/

 // This struct holds the BC interpolation weights for a 4x4 texel tile
struct BCWeights : IDifferentiable
{
    float w[16];

    __init(float _w)
    {
        for (int i = 0; i < 16; i++)
            w[i] = _w;
    }

    __init(float _w[16])
    {
        for (int i = 0; i < 16; i++)
            w[i] = _w[i];
    }

    [mutating]
    void clamp()
    {
        for (int i = 0; i < 16; i++)
            w[i] = clamp(w[i], 0, 1);
    }

    [mutating]
    void quantize()
    {
        // 3b for the 1st weight
        w[0] = round(w[0] * 7) / 7;
        // and 4b for the others
        for (int i = 1; i < 15; i++)
            w[i] = round(w[i] * 15) / 15;
    }

    BCWeights rsqrt()
    {
        BCWeights r;
        for (int i = 0; i < 16; i++)
            r.w[i] = rsqrt(w[i]);
        return r;
    }
}

BCWeights operator+(float a, BCWeights b)
{
    BCWeights r;
    for (int i = 0; i < 16; i++)
        r.w[i] = a + b.w[i];
    return r;
}

BCWeights operator+(BCWeights a, BCWeights b)
{
    BCWeights r;
    for (int i = 0; i < 16; i++)
        r.w[i] = a.w[i] + b.w[i];
    return r;
}

BCWeights operator-(BCWeights a, BCWeights b)
{
    BCWeights r;
    for (int i = 0; i < 16; i++)
        r.w[i] = a.w[i] - b.w[i];
    return r;
}

BCWeights operator*(BCWeights a, BCWeights b)
{
    BCWeights r;
    for (int i = 0; i < 16; i++)
        r.w[i] = a.w[i] * b.w[i];
    return r;
}

BCWeights operator*(float a, BCWeights b)
{
    BCWeights r;
    for (int i = 0; i < 16; i++)
        r.w[i] = a * b.w[i];
    return r;
}

 // This struct holds all of the data for a 4x4 texel tile
struct BCTile : IDifferentiable
{
    BCWeights weights;
    float4 minEndPoints;
    float4 maxEndPoints;

    __init()
    {
        weights = BCWeights(0.f);
        minEndPoints = 0.f;
        maxEndPoints = 0.f;
    }

    __init(BCTile.Differential tile)
    {
        weights = BCWeights(tile.weights.w);
        minEndPoints = tile.minEndPoints;
        maxEndPoints = tile.maxEndPoints;
    }

    __init(float _weight, float4 _minEndPoints, float4 _maxEndPoints)
    {
        weights = BCWeights(_weight);
        minEndPoints = _minEndPoints;
        maxEndPoints = _maxEndPoints;
    }

    [mutating]
    void clamp()
    {
        weights.clamp();
        minEndPoints = clamp(minEndPoints, 0, 1);
        maxEndPoints = clamp(maxEndPoints, 0, 1);
    }

    [mutating]
    void quantize()
    {
        weights.quantize();
        minEndPoints = round(minEndPoints * 127) / 127;
        maxEndPoints = round(maxEndPoints * 127) / 127;
    }

    BCTile rsqrt()
    {
        BCTile r;
        r.weights = weights.rsqrt();
        r.minEndPoints = rsqrt(minEndPoints);
        r.maxEndPoints = rsqrt(maxEndPoints);
        return r;
    }

    [BackwardDifferentiable]
    float4 decode(int i)
    {
        return (1 - weights.w[i]) * minEndPoints + weights.w[i] * maxEndPoints;
    }

    // Use 6b interpolation to match API specs
    float4 decodeExact(int i)
    {
        let b = uint(round(64.f * saturate(weights.w[i])));
        let a = 64u - b;
        uint4 uMinEndPoints = uint4(127.f * minEndPoints) << 1u;
        uint4 uMaxEndPoints = uint4(127.f * maxEndPoints) << 1u;
        return (32u + (a * uMinEndPoints + b * uMaxEndPoints) >> 6) / 255.f;
    }
}

BCTile operator*(BCTile a, BCTile b)
{
    BCTile r;
    r.weights = a.weights * b.weights;
    r.minEndPoints = a.minEndPoints * b.minEndPoints;
    r.maxEndPoints = a.maxEndPoints * b.maxEndPoints;
    return r;
}

BCTile operator*(float a, BCTile b)
{
    BCTile r;
    r.weights = a * b.weights;
    r.minEndPoints = a * b.minEndPoints;
    r.maxEndPoints = a * b.maxEndPoints;
    return r;
}

BCTile operator+(float a, BCTile b)
{
    BCTile r;
    r.weights = a + b.weights;
    r.minEndPoints = a + b.minEndPoints;
    r.maxEndPoints = a + b.maxEndPoints;
    return r;
}

BCTile operator+(BCTile a, BCTile b)
{
    BCTile r;
    r.weights = a.weights + b.weights;
    r.minEndPoints = a.minEndPoints + b.minEndPoints;
    r.maxEndPoints = a.maxEndPoints + b.maxEndPoints;
    return r;
}

BCTile operator-(BCTile a, BCTile b)
{
    BCTile r;
    r.weights = a.weights - b.weights;
    r.minEndPoints = a.minEndPoints - b.minEndPoints;
    r.maxEndPoints = a.maxEndPoints - b.maxEndPoints;
    return r;
}

struct Params {
    int useAdam;
    int numOptimizationSteps;
    float lr; // learning rate
    float adamBeta1;
    float adamBeta2;
    uint2 textureDim;
}
[[vk::push_constant]] Params gParams;

// Uncompressed input texture
Texture2D<float4> gInputTex;

// Decoded BC texture
RWTexture2D<float4> gDecodedTex;

[BackwardDifferentiable]
void l2Loss(BCTile tile, no_diff float4 target[16], out float loss[16])
{
    [ForceUnroll]
    for (int i = 0; i < 16; i++)
    {
        let delta = target[i] - tile.decode(i);
        loss[i] = dot(delta, delta);
    }
}

[numthreads(4, 8, 1)]
void encoder(int3 dispatchThreadId : SV_DispatchThreadID)
{
    let texel = 4 * dispatchThreadId.xy;
    if (any(texel >= gParams.textureDim.xy)) return;

    // Adam moments
    BCTile m1, m2;

    // Load target values and initialize with mid-point interpolation
    // and minmax endpoints computed over the tile
    float4 target[16];
    BCTile tile = BCTile(0.5, float4(1), float4(0));
    for (int i = 0; i < 16; i++)
    {
        target[i] = gInputTex[texel + int2(i % 4, i / 4)];
        tile.minEndPoints = min(tile.minEndPoints, target[i]);
        tile.maxEndPoints = max(tile.maxEndPoints, target[i]);
    }

    // To compress the texture we perform a number of gradient descent steps
    // to joint-optimize interpolation weights and endpoints for each tile.
    // Eventually the interpolation weights are quantized and frozen,
    // while the endpoints are fine tuned for a few more steps.
    bool frozenWeight = false;
    let endPointsFineTuninigThres = 0.85f;
    for (int s = 0; s < gParams.numOptimizationSteps; s++)
    {
        // Forward pass - compute error
        float loss[16];
        l2Loss(tile, target, loss);

        // Backward pass - compute gradients
        var dp_tile = diffPair(tile);
        bwd_diff(l2Loss)(dp_tile, target, {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1});

        // Use Adam optimizer to update gradients
        BCTile grad = BCTile(dp_tile.d);
        if (gParams.useAdam != 0)
        {
            m1 = gParams.adamBeta1 * m1 + (1 - gParams.adamBeta1) * grad;
            m2 = gParams.adamBeta2 * m2 + (1 - gParams.adamBeta2) * (grad * grad);
            let unbiased_m1 = rcp(1.f - pow(gParams.adamBeta1, 1 + s)) * m1;
            let unbiased_m2 = rcp(1.f - pow(gParams.adamBeta2, 1 + s)) * m2;
            grad = unbiased_m1 * (1E-10f + unbiased_m2).rsqrt();
        }

        // If weights are frozen zero their gradient
        if (frozenWeight)
            grad.weights = BCWeights(0);

        // Update BC tile via gradient descent & clamp its values to a valid range
        tile = tile - gParams.lr * grad;
        tile.clamp();

        // Time to freeze the BC weights?
        if (frozenWeight == false && float(s) / gParams.numOptimizationSteps >= endPointsFineTuninigThres)
        {
            // Quantize & freeze weights
            // We will continue to fine tune the endpoints for a few more steps
            frozenWeight = true;
            tile.weights.quantize();
        }
    }

    // Quantize & write out decoded tile
    tile.quantize();
    for (int i = 0; i < 16; i++)
        gDecodedTex[texel + int2(i % 4, i / 4)] = tile.decodeExact(i);;
}
