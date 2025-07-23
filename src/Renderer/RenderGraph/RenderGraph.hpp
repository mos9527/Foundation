#pragma once
#include <RHICore/Common.hpp>
#include "Resource.hpp"
/*
Acyclic render dependency graph implementation with automatic resource barrier
and aliasing/reuse.

References:
- https://levelup.gitconnected.com/organizing-gpu-work-with-directed-acyclic-graphs-f3fd5f2c2af3
- https://epicgames.ent.box.com/s/ul1h44ozs0t2850ug0hrohlzm53kxwrz
- https://www.gdcvault.com/play/1024656/Advanced-Graphics-Tech-Moving-to
- https://logins.github.io/graphics/2021/05/31/RenderGraphs.html
- https://dev.epicgames.com/documentation/en-us/unreal-engine/render-dependency-graph-in-unreal-engine
*/
namespace Foundation::Renderer {
    class RenderGraph;
}
