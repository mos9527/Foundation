#pragma once
#include "../Component.hpp"

struct CameraComponent : public SceneComponent {	
	float fov;
	float nearZ;
	float farZ;

	matrix view;
	matrix projection;
	matrix viewProjection;
	
	bool orthographic = false;

	CameraComponent(entt::entity ent) : SceneComponent(ent) {
		localTransform = AffineTransform::Identity;
		fov = XM_PIDIV4;
		nearZ = 0.01f;
		farZ = 100.0f;
	}
#ifdef IMGUI_ENABLED
	virtual void OnImGui() {
		ImGui::SliderFloat("FOV", &fov, 0.0f, XM_PIDIV2);
		ImGui::SliderFloat("Near Z", &nearZ, 0.0f, 100.0f);
		ImGui::SliderFloat("Far Z", &farZ, 0.0f, 100.0f);
		ImGui::Checkbox("Orthograhpic", &orthographic);					
	}
#endif
	SceneCamera get_struct(float aspect, AffineTransform globalTransform) {
		SceneCamera camera;
		Vector3 lookDirection{0,0,1}, upDirection{0,1,0};		
		lookDirection = Vector3::TransformNormal(lookDirection, globalTransform);
		upDirection = Vector3::TransformNormal(upDirection, globalTransform);
		view = XMMatrixLookToLH(globalTransform.Translation(), lookDirection , upDirection);
		fov = std::max(fov, 0.01f);
		if (orthographic) {
			projection = XMMatrixOrthographicLH(
				2 * tan(fov / 2) * aspect,
				2 * tan(fov / 2),
#ifdef INVERSE_Z
				farZ, nearZ
#else
				nearZ, farZ
#endif
			);
		}
		else {
			projection = XMMatrixPerspectiveFovLH(
				fov, aspect, 
#ifdef INVERSE_Z
				farZ, nearZ
#else
				nearZ, farZ
#endif
			);
		}
		viewProjection = view * projection;
		// 6 clipping planes
		// https://github.com/microsoft/DirectX-Graphics-Samples/blob/master/Samples/Desktop/D3D12MeshShaders/src/DynamicLOD/D3D12DynamicLOD.cpp#L475
		XMMATRIX vp = viewProjection.Transpose();
		camera.clipPlanes[0] = XMPlaneNormalize(XMVectorAdd(vp.r[3], vp.r[0])),      // Left
			camera.clipPlanes[1] = XMPlaneNormalize(XMVectorSubtract(vp.r[3], vp.r[0])), // Right
			camera.clipPlanes[2] = XMPlaneNormalize(XMVectorAdd(vp.r[3], vp.r[1])),      // Bottom
			camera.clipPlanes[3] = XMPlaneNormalize(XMVectorSubtract(vp.r[3], vp.r[1])), // Top
			camera.clipPlanes[4] = XMPlaneNormalize(vp.r[2]),                            // Near
			camera.clipPlanes[5] = XMPlaneNormalize(XMVectorSubtract(vp.r[3], vp.r[2])); // Far			
		// matrices
		camera.view = view.Transpose();
		camera.projection = projection.Transpose();
		camera.viewProjection = viewProjection.Transpose();
		camera.invView = view.Invert().Transpose();
		camera.invProjection = projection.Invert().Transpose();
		camera.invViewProjection = viewProjection.Invert().Transpose();
		// others		
		Vector3 translation = globalTransform.Translation();
		camera.position = { translation.x, translation.y,translation.z,1 };
		camera.fov = fov;
		camera.aspect = aspect;
		camera.nearZ = nearZ;
		camera.farZ = farZ;
		return camera;
	}	
};

template<> struct SceneComponentTraits<CameraComponent> {
	static constexpr SceneComponentType type = SceneComponentType::Camera;
	const char* name = "Camera";
};