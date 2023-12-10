#pragma once
#include "Renderer.hpp"
#include "RenderPass/Clear.hpp"
#include "RenderPass/InstanceCull.hpp"
#include "RenderPass/GBuffer.hpp"
#include "RenderPass/DeferredLighting.hpp"
#include "RenderPass/HiZ.hpp"
#include "RenderPass/Transparency.hpp"
#include "RenderPass/SilhouettePass.hpp"
#include "RenderPass/Skybox.hpp"
#include "RenderPass/Tonemapping.hpp"
using namespace EditorGlobals;

class DeferredRenderer {
public:
	RHI::ShaderResourceView* r_frameBufferSRV, * r_materialBufferSRV;
	RHI::Texture* r_frameBufferTex, * r_materialBufferTex;
	DeferredRenderer(RHI::Device* device) : 
		device(device), pass_Clear(device), pass_IndirectCull(device), pass_GBuffer(device), pass_HiZ(device), pass_Lighting(device),
		pass_Transparency(device), pass_Silhouette(device), pass_Skybox(device), pass_Tonemapping(device)
	{};	
	void Render(SceneView* sceneView);
private:	
	RHI::Device* const device;

	RenderGraphResourceCache cache;
	
	ClearPass pass_Clear;
	InstanceCull pass_IndirectCull;
	GBufferPass pass_GBuffer;
	HierarchalDepthPass pass_HiZ;
	DeferredLightingPass pass_Lighting;
	SkyboxPass pass_Skybox;
	TransparencyPass pass_Transparency;
	TonemappingPass pass_Tonemapping;
	SilhouettePass pass_Silhouette;
};