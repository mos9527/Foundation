#pragma once
#include <RenderCore/RenderResource.hpp>
#include <RHICore/Common.hpp>

namespace Foundation::RenderCore
{
class Renderer;
}

namespace EditorGizmos
{
void InsertPass(Renderer* renderer, ResourceHandle depthTexture, RHIExtent2D extent);

void BuildLightGizmos();
} // namespace EditorGizmos
