#pragma once
#include <Math/ModelViewProjection.hpp>
#include "Renderer.hpp"
#include "ImGui.hpp"
enum FEditorState
{
    FEInitEnter,
    FEInit,
    FERunningEnter,
    FERunning
};

struct FArcballCamera
{
    static constexpr char kControlsText[] = "Mouse Left: Rotate | Mouse Right: Pan | Mouse Wheel: Zoom";

    float3 center, position;
    float radius;
    quat rot;
    float zNear, fovY, aspect;
    mat4 view, proj;
    bool Update(SDL_Event const& event)
    {
        bool updated = false;
        if (event.type == SDL_EVENT_MOUSE_MOTION)
        {
            if (event.motion.state & SDL_BUTTON_LMASK)
            {
                float yawDelta = -event.motion.xrel * 1e-2f;
                float pitchDelta = -event.motion.yrel * 1e-2f;
                quat yawRot = angleAxis(yawDelta, vec3(0, 1, 0));
                quat pitchRot = angleAxis(pitchDelta, vec3(1, 0, 0));
                rot = normalize(yawRot * rot * pitchRot);
                updated = true;
            }
            if (event.motion.state & SDL_BUTTON_RMASK)
            {
                vec3 right = rot * vec3(1, 0, 0);
                vec3 up = rot * vec3(0, 1, 0);
                center -= right * (event.motion.xrel * radius * 1e-3f);
                center += up * (event.motion.yrel * radius * 1e-3f);
                updated = true;
            }
        }
        if (event.type == SDL_EVENT_MOUSE_WHEEL)
        {
            radius -= event.wheel.y * radius * 1e-1f;
            radius = radius < 1e-3f ? 1e-3f : radius;
            updated = true;
        }
        // ---
        proj = infinitePerspectiveRHReverseZ(fovY, aspect, zNear);
        vec3 dir = rot * vec3(0, 0, 1);
        position = center + radius * dir;
        view = viewMatrixRHReverseZ(position, rot);
        return updated;
    }
};
