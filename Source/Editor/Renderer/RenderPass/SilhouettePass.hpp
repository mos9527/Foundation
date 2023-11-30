#pragma once
#include "../Renderer.hpp"

class SilhouettePass {
	std::unique_ptr<RHI::Shader> VS, PS,CS;
	std::unique_ptr<RHI::RootSignature> RS, blendRS;
	std::unique_ptr<RHI::PipelineState> PSO, PSO_Blend;
	std::unique_ptr<RHI::CommandSignature> IndirectCmdSig;
public:
	struct SilhouettePassHandles {
		RgHandle& silhouetteCMD;
		RgHandle& silhouetteCMDUAV;

		RgHandle& silhouetteDepth;
		RgHandle& silhouetteDepthDSV;
		RgHandle& silhouetteDepthSRV;

		RgHandle& frameBuffer;
		RgHandle& frameBufferUAV;
	};

	SilhouettePass(RHI::Device* device);
	RenderGraphPass& insert(RenderGraph& rg, SceneView* sceneView, SilhouettePassHandles&& handles);
};