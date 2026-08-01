import ICommon;

uniform UBO globalParams;

#include "ISharcIntegration.slang"

[shader("compute")]
[numthreads(256, 1, 1)]
void ComputeMain(uint3 tid : SV_DispatchThreadID)
{
    SharcResolveParameters resolveParameters;
    resolveParameters.cameraPositionPrev = globalParams.sharcPreviousCameraPosition.xyz;
    resolveParameters.accumulationFrameNum = globalParams.sharcAccumulationFrames;
    resolveParameters.responsiveFrameNum = globalParams.sharcAccumulationFrames;
    resolveParameters.staleFrameNumMax = globalParams.sharcStaleFrames;
    resolveParameters.frameIndex = globalParams.frameNumber;
    SharcResolveEntry(tid.x, MakeSharcParameters(), resolveParameters);
}
