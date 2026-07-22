#pragma once
#include <Math/Math.hpp>
#include <RenderCore/Renderer.hpp>
#include <RHICore/Common.hpp>

namespace Foundation::RenderCore
{
class Renderer;
}

namespace EditorGizmos
{
void InsertPass(Renderer* renderer, ResourceHandle depthTexture, RHIExtent2D extent);

void BuildLightGizmos();

// Scene light index at render-pixel coordinates, or -1 if none.
int PickLightAtRenderPixel(Math::int2 pixel);

void Shutdown();
} // namespace EditorGizmos
