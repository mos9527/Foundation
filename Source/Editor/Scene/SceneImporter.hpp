#pragma once
#include "../../Common/IO.hpp"
#include "../../Common/Task.hpp"
#include "Scene.hpp"

struct aiScene;
struct SceneImporter {
public:
	struct SceneImporterAtomicStatus {
		std::atomic<bool> uploadComplete;
		std::atomic<bool> loadComplete;
		std::atomic<uint> numUploaded;
		std::atomic<uint> numToUpload;
		// xxx are there alernative ways for cross-threaed syncs?
	};
private:
	static void load_aiScene(UploadContext* ctx, SceneImporterAtomicStatus& statusOut, Scene& sceneOut, const aiScene* scene, path_t sceneFolder);
public:
	static void load(UploadContext* ctx, SceneImporterAtomicStatus& statusOut, Scene& sceneOut, path_t sceneFile);
};
