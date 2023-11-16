#pragma once
#include "../Component.hpp"

struct CameraComponent : public SceneComponent {	
	float4 FovAspectNearZFarZ;
	float4 clipPlanes[6];

	matrix view;
	matrix projection;
	matrix viewProjection;
	
	CameraComponent() {
		localTransform.translation = { 0,0,-1 };
		localTransform.rotation.SetRotationPitchYawRoll({ 0,0,0 });
		FovAspectNearZFarZ = { XM_PIDIV4, 1.0f, 0.01f, 100.0f };
		update();
	}
	void update() {
		view = XMMatrixLookToLH(
			localTransform.translation, localTransform.rotation.GetDirectionVector(), XMVector3Rotate({ 0,1,0 }, localTransform.rotation)
		);
		projection = XMMatrixPerspectiveFovLH(
			FovAspectNearZFarZ.x, FovAspectNearZFarZ.y, FovAspectNearZFarZ.z, FovAspectNearZFarZ.w
		);
		viewProjection = view * projection;
		// 6 clipping planes
		// https://github.com/microsoft/DirectX-Graphics-Samples/blob/master/Samples/Desktop/D3D12MeshShaders/src/DynamicLOD/D3D12DynamicLOD.cpp#L475
		XMMATRIX vp = viewProjection.Transpose();
		clipPlanes[0] = XMPlaneNormalize(XMVectorAdd(vp.r[3], vp.r[0])),      // Left
			clipPlanes[1] = XMPlaneNormalize(XMVectorSubtract(vp.r[3], vp.r[0])), // Right
			clipPlanes[2] = XMPlaneNormalize(XMVectorAdd(vp.r[3], vp.r[1])),      // Bottom
			clipPlanes[3] = XMPlaneNormalize(XMVectorSubtract(vp.r[3], vp.r[1])), // Top
			clipPlanes[4] = XMPlaneNormalize(vp.r[2]),                            // Near
			clipPlanes[5] = XMPlaneNormalize(XMVectorSubtract(vp.r[3], vp.r[2])); // Far	
	}
};