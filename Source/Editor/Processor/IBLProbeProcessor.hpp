#pragma once
#include "Processor.hpp"
#include "ProcessPass/IBLPrefilter.hpp"
struct IBLProbeProcessorEvents {
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
enum class IBLProbeProcessorStates {
	IdleNoProbe,
	Processing,
	IdleWithProbe
};
struct IBLProbeProcesserState : public FSM::EFSM<IBLProbeProcessorStates, IBLProbeProcessorEvents, IBLProbeProcessorStates::IdleNoProbe> {
	const char* currentProcess = nullptr;
	uint numProcessed = 0;
	uint numToProcess = 0;

	void fail(IBLProbeProcessorEvents event) {};
	IBLProbeProcessorStates transition(IBLProbeProcessorEvents event) {
		using enum IBLProbeProcessorEvents::Type;
		using enum IBLProbeProcessorStates;
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
class IBLProbeProcessor {
public:
	uint dimension, numMips;
	std::unique_ptr<RHI::Texture> cubeMap, irridanceMap, radianceMapArray, lutArray;
	std::vector<std::unique_ptr<RHI::UnorderedAccessView>> cubeMapUAVs, radianceCubeArrayUAVs;
	std::unique_ptr<RHI::UnorderedAccessView> irridanceCubeUAV, lutArrayUAV;
	std::unique_ptr<RHI::ShaderResourceView> cubemapSRV, irridanceCubeSRV, radianceCubeArraySRV, lutArraySRV;

	IBLProbeProcesserState state;
	IBLProbeProcessor(RHI::Device* device, uint dimesnion);	
	void ProcessAsync(TextureAsset* srcImage);
private:
	void Process(TextureAsset* srcImage);
	
	RHI::Device* const device;
	
	DefaultTaskThreadPool taskpool;
	RenderGraphResourceCache cache;

	IBLPrefilterPass proc_Prefilter;
};
