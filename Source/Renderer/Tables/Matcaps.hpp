// Bundled matcap presets
#pragma once
#include <iterator>

struct MatcapEntry
{
    const char* label;
    const char* path;
};

inline constexpr MatcapEntry kMatcaps[] = {
    {"Basic 1", "Data/Assets/Matcaps/basic_1.png"},
    {"Basic 2", "Data/Assets/Matcaps/basic_2.png"},
    {"Basic Dark", "Data/Assets/Matcaps/basic_dark.png"},
    {"Basic Side", "Data/Assets/Matcaps/basic_side.png"},
    {"Ceramic Dark", "Data/Assets/Matcaps/ceramic_dark.png"},
    {"Ceramic Lightbulb", "Data/Assets/Matcaps/ceramic_lightbulb.png"},
    {"Check Normal +Y", "Data/Assets/Matcaps/check_normal+y.png"},
    {"Check Rim Dark", "Data/Assets/Matcaps/check_rim_dark.png"},
    {"Check Rim Light", "Data/Assets/Matcaps/check_rim_light.png"},
    {"Clay Brown", "Data/Assets/Matcaps/clay_brown.png"},
    {"Clay Muddy", "Data/Assets/Matcaps/clay_muddy.png"},
    {"Clay Studio", "Data/Assets/Matcaps/clay_studio.png"},
    {"Jade", "Data/Assets/Matcaps/jade.png"},
    {"Metal Anisotropic", "Data/Assets/Matcaps/metal_anisotropic.png"},
    {"Metal Carpaint", "Data/Assets/Matcaps/metal_carpaint.png"},
    {"Metal Lead", "Data/Assets/Matcaps/metal_lead.png"},
    {"Metal Shiny", "Data/Assets/Matcaps/metal_shiny.png"},
    {"Pearl", "Data/Assets/Matcaps/pearl.png"},
    {"Reflection Check Horizontal", "Data/Assets/Matcaps/reflection_check_horizontal.png"},
    {"Reflection Check Vertical", "Data/Assets/Matcaps/reflection_check_vertical.png"},
    {"Resin", "Data/Assets/Matcaps/resin.png"},
    {"Skin", "Data/Assets/Matcaps/skin.png"},
    {"Toon", "Data/Assets/Matcaps/toon.png"},
    {"Flat", "Data/Assets/Matcaps/flat.png"},
};

inline constexpr int kMatcapCount = std::size(kMatcaps);
inline constexpr int kDefaultMatcap = 11;
