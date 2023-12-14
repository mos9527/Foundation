#pragma once
#include "Processor.hpp"
#include "ProcessPass/IBLPrefilter.hpp"
struct HDRIProbeProcessorEvents {
	enum class Type {
		Begin,
		Process,
		End
	} type;
	union
	{
		struct ProcessingEvent {
			const char* processName;
			uint newNumProcessed;
			uint newNumToProcess;
		} proceesEvent;
	};
};
enum class HDRIProbeProbeProcessorStates {
	IdleNoProbe,
	Processing,
	IdleWithProbe
};
struct HDRIProbeProbeProcesserState : public FSM::EFSM<HDRIProbeProbeProcessorStates, HDRIProbeProcessorEvents, HDRIProbeProbeProcessorStates::IdleNoProbe> {
	const char* currentProcess = nullptr;
	uint numProcessed = 0;
	uint numToProcess = 0;

	void fail(HDRIProbeProcessorEvents event) {};
	HDRIProbeProbeProcessorStates transition(HDRIProbeProcessorEvents event) {
		using enum HDRIProbeProcessorEvents::Type;
		using enum HDRIProbeProbeProcessorStates;
		switch (event.type)
		{
		case Begin:
			if (state == IdleNoProbe || state == IdleWithProbe) {
				state = Processing;
				numProcessed = numToProcess = 0;
			}
			else fail(event);
			break;
		case Process:
			if (state == Processing) {
				currentProcess = event.proceesEvent.processName;
				numProcessed += event.proceesEvent.newNumProcessed;
				numToProcess += event.proceesEvent.newNumToProcess;
			}
			else fail(event);
			break;
		case End:
			if (state == Processing) state = IdleWithProbe;
			else fail(event);
			break;
		}
		return state;
	}
};
class HDRIProbeProcessor {
public:
	uint dimension, numMips;
	std::unique_ptr<RHI::Texture> cubeMap, irridanceMap, radianceMapArray;
	std::vector<std::unique_ptr<RHI::UnorderedAccessView>> cubeMapUAVs, radianceCubeArrayUAVs;
	std::unique_ptr<RHI::UnorderedAccessView> irridanceCubeUAV;
	std::unique_ptr<RHI::ShaderResourceView> cubemapSRV, irridanceCubeSRV, radianceCubeArraySRV;

	HDRIProbeProbeProcesserState state;
	HDRIProbeProcessor(RHI::Device* device, uint dimesnion);	
	void ProcessAsync(TextureAsset* srcImage);
	void Process(TextureAsset* srcImage);
private:
	
	RHI::Device* const device;
	
	DefaultTaskThreadPool taskpool;
	RenderGraphResourceCache cache;

	IBLPrefilterPass proc_Prefilter;
};
