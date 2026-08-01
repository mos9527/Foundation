#ifndef SHARC_UPDATE
#define SHARC_UPDATE 0
#endif
#ifndef SHARC_QUERY
#define SHARC_QUERY 0
#endif

#define SHARC_ENABLE_GLSL 0
#define SHARC_MATERIAL_DEMODULATION 1
#define SHARC_ENABLE_SH_ENCODING 0
#define SHARC_ENABLE_RESPONSIVE_LIGHTING 0
#define HASH_GRID_COMPACT 1

#include "ISharc.slang"

RWStructuredBuffer<uint> sharcHashEntries;
RWStructuredBuffer<SharcPackedData> sharcResolved;
#if !SHARC_QUERY
RWStructuredBuffer<SharcAccumulationData> sharcAccumulation;
#endif

SharcParameters MakeSharcParameters()
{
    SharcParameters parameters;
    parameters.hashGridParameters.cameraPosition = globalParams.camPosition.xyz;
    parameters.hashGridParameters.logarithmBase = SHARC_GRID_LOGARITHM_BASE;
    parameters.hashGridParameters.sceneScale = globalParams.sharcSceneScale;
    parameters.hashGridParameters.levelBias = SHARC_GRID_LEVEL_BIAS;
    parameters.hashGridData.capacity = globalParams.sharcEntries;
    parameters.hashGridData.hashEntriesBuffer = sharcHashEntries;
    parameters.resolvedBuffer = sharcResolved;
#if !SHARC_QUERY
    parameters.accumulationBuffer = sharcAccumulation;
#endif
    parameters.radianceScale = globalParams.sharcRadianceScale;
    return parameters;
}
