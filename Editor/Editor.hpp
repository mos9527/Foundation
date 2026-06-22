#pragma once
#include <Math/ModelViewProjection.hpp>
#include "ImGui.hpp"
#include <Renderer/Renderer.hpp>
enum FEditorState
{
    FEInitEnter,
    FENoScene, // no scene loaded: render ImGui only until a scene is installed
    FERunningEnter,
    FERunning,
    FERenderingEnter,
    FERendering
};

