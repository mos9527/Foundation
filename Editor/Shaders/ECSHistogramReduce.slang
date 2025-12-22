#include "ICommon.slang"

uniform UBO globalParams;
RWStructuredBuffer<uint> bins;
RWStructuredBuffer<float> output; // Final scene avg. luminance
groupshared Atomic<uint> countGS, weightGS;
[shader("compute")]
[numthreads(64, 1, 1)]
void reduce(uint gid : SV_GroupIndex, uint groupID : SV_GroupID) {
    countGS.store(0u); weightGS.store(0u);
    GroupMemoryBarrierWithGroupSync();
    // Drop extremities
    bool keep = (gid >= 2 && gid <= 48);
    uint count = bins[gid] * keep;
    uint weighted = count * gid;
    uint countWave = WaveActiveSum(count);
    uint weightWave = WaveActiveSum(weighted);
    if (WaveIsFirstLane()){
        countGS.add(countWave);
        weightGS.add(weightWave);
    }
    GroupMemoryBarrierWithGroupSync();
    if (gid == 0) {
        // sum = Num_i * (EV_i - min)/(max-min)*64
        uint countAll = countGS.load(); // No. samples in range
        uint weightAll = weightGS.load(); // Weighted samples sum
        float p = (float)weightAll / (countAll ? countAll : 1u);
        float meanEV = p / 64.0f * (globalParams.camMaxEV - globalParams.camMinEV) + globalParams.camMinEV;
        float meanLuma = exp2(meanEV);
        float meanLumaPrev = output[0];
        meanLumaPrev = isnan(meanLumaPrev) ? meanLuma : meanLumaPrev;
        float lumaAdapted = meanLumaPrev + (meanLuma - meanLumaPrev) * clamp(globalParams.camAdaptCoeff,0.0f,1.0f);
        output[0] = lumaAdapted;
        bins[gid] = 0; // Clear for next frame
    }
}
