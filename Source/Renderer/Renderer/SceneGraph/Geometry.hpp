#pragma once
#include "../Common.hpp"
#include "../RHI/RHI.hpp"
#include "../../IO/Mesh.hpp"
#include "Structs.hpp"
class Renderer;
struct Geometry { // xxx : SceneObject base
private:
	size_t allocSize;
	std::shared_ptr<RHI::Buffer> intermediateBuffer;
	entt::entity geometryBuffer = entt::tombstone;
	GeometryBufferOffsets bufferOffsets;	
public:
	RHI::name_t name = L"Geometry";
	
	size_t get_buffer_size() { return allocSize; }
	void load_static_mesh(Renderer* renderer, IO::mesh_static const& mesh);	
	entt::entity upload(Renderer* renderer, RHI::CommandList* cmdList);
	bool is_uploaded() { return geometryBuffer != entt::tombstone; }	
	auto get_buffer() { return geometryBuffer; }
	auto get_buffer_offsets() { return bufferOffsets; }
};