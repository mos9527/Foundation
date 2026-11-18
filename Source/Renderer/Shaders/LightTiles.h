#pragma once
#ifdef __cplusplus
#include <cstdint>
inline constexpr uint32_t kRasterLightTileWidth = 16u;
inline constexpr uint32_t kRasterLightTileCapacity = 256u;
#else
public static const uint kRasterLightTileWidth = 16u;
public static const uint kRasterLightTileCapacity = 256u;
#endif
