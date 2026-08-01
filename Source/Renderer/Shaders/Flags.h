#pragma once

#ifdef __cplusplus
#include <Core/Enums.hpp>
#define SHARED_ENUM_BEGIN(T, INT_T) \
enum class T : INT_T; \
inline constexpr INT_T to_integer(T e) { return static_cast<INT_T>(e); } \
enum class T : INT_T {
#define SHARED_ENUM_END() };
#define SHARED_BITMASK_ENUM_BEGIN(T, INT_T) BITMASK_ENUM_BEGIN(T, INT_T)
#define SHARED_BITMASK_ENUM_END() BITMASK_ENUM_END()
#else
#define SHARED_ENUM_BEGIN(T, INT_T) public enum class T : INT_T {
#define SHARED_ENUM_END() };
#define SHARED_BITMASK_ENUM_BEGIN(T, INT_T) public enum class T##Bits : INT_T {
#define SHARED_BITMASK_ENUM_END() };
#endif

SHARED_ENUM_BEGIN(PTSampler, uint32_t)
    PCG = 0u,
    Sobol = 1u
SHARED_ENUM_END()

SHARED_ENUM_BEGIN(LightSampler, uint32_t)
    BVH = 0u,
    Uniform = 1u
SHARED_ENUM_END()

SHARED_ENUM_BEGIN(CameraProjection, uint32_t)
    Perspective = 0u,
    Panoramic = 1u
SHARED_ENUM_END()

SHARED_BITMASK_ENUM_BEGIN(ViewFlags, uint32_t)
    Overdraw = 1u << 0,
    Meshlet = 1u << 1,
    BaseColor = 1u << 2,
    Normal = 1u << 3,
    Position = 1u << 5,
    Matcap = 1u << 6,
    TextureLOD = 1u << 7,
    AOVDiffuse = 1u << 8,
    AOVSpecular = 1u << 9,
    AOVSampleCount = 1u << 10,
    SHARCGrid = 1u << 11,
    SHARCOccupancy = 1u << 12,
    SHARCRadiance = 1u << 13,
    EnableRasterRTShadows = 1u << 16,
    EnableRasterAmbientOcclusion = 1u << 17,
    ForceTextureLOD0 = 1u << 24
SHARED_BITMASK_ENUM_END()

SHARED_BITMASK_ENUM_BEGIN(MaterialFlags, uint32_t)
    DbgWhiteBaseColor = 1u << 0
SHARED_BITMASK_ENUM_END()

SHARED_BITMASK_ENUM_BEGIN(CullFlags, uint32_t)
    Frustum = 1u << 0,
    Occlusion = 1u << 1,
    Backface = 1u << 2,
    StageEarly = 1u << 16,
    StageLate = 1u << 17
SHARED_BITMASK_ENUM_END()

SHARED_BITMASK_ENUM_BEGIN(PTCompileOptions, uint32_t)
    SamplerSobol = 1u << 1,
    SamplerPCG = 1u << 2,
    ForceTextureLOD0 = 1u << 3,
    LightSamplerUniform = 1u << 4,
    EnergyCompensation = 1u << 5
SHARED_BITMASK_ENUM_END()

SHARED_BITMASK_ENUM_BEGIN(GSInstanceFlags, uint32_t)
    Dynamic = 0x100u
SHARED_BITMASK_ENUM_END()

SHARED_BITMASK_ENUM_BEGIN(GSLightFlags, uint32_t)
    TwoSided = 0x100u,
    UseShadow = 0x200u,
    EnvironmentMap = 0x400u
SHARED_BITMASK_ENUM_END()

#undef SHARED_BITMASK_ENUM_BEGIN
#undef SHARED_BITMASK_ENUM_END
#undef SHARED_ENUM_BEGIN
#undef SHARED_ENUM_END
