#pragma once
#include <Math/Math.hpp>
#include <Math/ModelViewProjection.hpp>
#include <SDL3/SDL.h>
using namespace Foundation::Math;

// Orbit camera with WASD fly-through. Shared by the editor and the renderer examples.
// Drives a right-handed, reverse-Z view/projection (matches the Foundation renderer UBO).
struct FArcballCamera
{
    static constexpr char kControlsText[] = "Mouse Left: Rotate | Mouse Right: Pan | Mouse Wheel: Zoom | WASD: Move | Shift: Fast | Space: Pause";

    float3 center, position;
    float radius;
    quat rot;
    float zNear, fovY, aspect;
    mat4 view, proj;
    float moveSpeed = 2.0f;
    // WASD key state
    bool keyW = false, keyA = false, keyS = false, keyD = false;
    bool keyShift = false;
    // Called every frame: continuously move center based on WASD state
    bool UpdateMovement(float dt)
    {
        vec3 moveDir(0.0f);
        vec3 forward = rot * vec3(0, 0, -1); // camera forward (note: view direction is -Z)
        vec3 right   = rot * vec3(1, 0, 0);
        if (keyW) moveDir += forward;
        if (keyS) moveDir -= forward;
        if (keyA) moveDir -= right;
        if (keyD) moveDir += right;
        if (glm::dot(moveDir, moveDir) > 1e-6f)
        {
            moveDir = glm::normalize(moveDir);
            float speed = moveSpeed * (keyShift ? 4.0f : 1.0f);
            center += moveDir * speed * dt;
            return true;
        }
        return false;
    }

    bool Update(SDL_Event const& event)
    {
        bool updated = false;
        if (event.type == SDL_EVENT_MOUSE_MOTION)
        {
            SDL_MouseButtonFlags mouseState = SDL_GetMouseState(NULL, NULL);
            if (mouseState & SDL_BUTTON_LMASK)
            {
                float yawDelta = -event.motion.xrel * 1e-2f;
                float pitchDelta = -event.motion.yrel * 1e-2f;
                quat yawRot = angleAxis(yawDelta, vec3(0, 1, 0));
                quat pitchRot = angleAxis(pitchDelta, vec3(1, 0, 0));
                rot = normalize(yawRot * rot * pitchRot);
                updated = true;
            }
            if (mouseState & SDL_BUTTON_RMASK)
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
