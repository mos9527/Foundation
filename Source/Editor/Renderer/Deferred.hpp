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
	{
		lastCheckTick = hires_seconds();
	};	
	void Render(SceneView* sceneView, RHI::CommandList* ctx);
	void CheckAndResetPassIfNecessary() {
#define DOCHECK(X) if (!X.is_all_shader_uptodate()) X.reset();
		if (hires_seconds() - lastCheckTick >= 1.0) {
			DOCHECK(pass_Clear);
			DOCHECK(pass_IndirectCull);
			DOCHECK(pass_GBuffer);
			DOCHECK(pass_HiZ);
			DOCHECK(pass_Lighting);
			DOCHECK(pass_Skybox);
			DOCHECK(pass_Transparency);
			DOCHECK(pass_Tonemapping);
			DOCHECK(pass_Silhouette);
			lastCheckTick = hires_seconds();
		}
	}
private:	
	double lastCheckTick{};

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